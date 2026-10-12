#include "../include/compiler.h"
#include "../include/target.h"
#include "../include/codegen_abi.h"
#include "../include/regalloc.h"
#include "../include/value_facts.h"
#include "../include/isel.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct { size_t locals, flags, args, bytes, maxargs, local_slots; } Layout;
typedef struct { FILE *out; size_t f,b,k; BPPTarget target; const BLIRFunction *fn; Layout layout; const BPPRegPlan *plan; BPPValueFacts facts; BPPValueFact *fact_regs,*fact_locals,*fact_globals; unsigned char *dirty, *before, *kind, *constant;unsigned *loaded_phys;long long *value;size_t near,emitted_spills,emitted_reloads,emitted_rematerializations;int asm_comments,instrument_native,native_release,truth_states;unsigned *dirty_phys; } Emitter;
static void sync_known(Emitter *e){for(size_t r=0;r<e->fn->register_count;++r){BPPValueFact f=e->fact_regs[r];e->kind[r]=f.kind==BPP_VALUE_INT?BPP_KIND_INT:f.kind==BPP_VALUE_NULL?BPP_KIND_NULL:f.kind>=BPP_VALUE_STRING?BPP_KIND_OBJECT:BPP_KIND_UNKNOWN;e->constant[r]=f.integer_known;e->value[r]=f.integer;}}
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
static void access_traffic(FILE *o,const BLIRFunction *fn){
    size_t loads=0,stores=0,branches=0,calls=0;
    for(size_t b=0;b<fn->block_count;++b)for(size_t k=0;k<fn->blocks[b].count;++k){BLIROp op=fn->blocks[b].insts[k].op;
        loads+=op==BL_IR_LOAD_LOCAL||op==BL_IR_LOAD_GLOBAL;
        stores+=op==BL_IR_INIT_LOCAL||op==BL_IR_INIT_GLOBAL||op==BL_IR_STORE_LOCAL||op==BL_IR_STORE_GLOBAL;
        branches+=op==BL_IR_JUMP||op==BL_IR_BRANCH;
        calls+=op==BL_IR_CALL;
    }
    fprintf(o,"    // B++ access-traffic function=%s slot-loads=%zu slot-stores=%zu branches=%zu helper-calls=%zu\n",fn->name,loads,stores,branches,calls);
}
static int layout(const BLIRFunction *fn,const BPPRegPlan *plan, Layout *l) {
    l->maxargs=0;
    for(size_t b=0;b<fn->block_count;++b) for(size_t k=0;k<fn->blocks[b].count;++k)
        if(fn->blocks[b].insts[k].arg_count>l->maxargs) l->maxargs=fn->blocks[b].insts[k].arg_count;
    l->local_slots=plan->local_slot_count;
    BPPNativeFrameLayout frame;
    if(!bpp_native_frame_layout(plan->cell_count,l->local_slots,l->maxargs,&frame))return 0;
    l->locals=frame.locals_offset;l->flags=frame.flags_offset;l->args=frame.outgoing_offset;l->bytes=frame.frame_bytes;return 1;
}
static size_t layout_flags(const BPPRegPlan *plan,const BLIRFunction *fn) {
    size_t flags=0;int identity=1;
    for(size_t r=0;r<fn->register_count;++r)if(plan->cell[r]!=r){identity=0;break;}
    if(!identity)flags|=BPP_LAYOUT_REGISTER_MAP;
    identity=1;
    for(size_t l=0;l<fn->local_count;++l)if(plan->local_map[l]!=l){identity=0;break;}
    if(!identity)flags|=BPP_LAYOUT_LOCAL_MAP;
    return flags;
}
static size_t arm_saved_registers(const BPPRegPlan *plan,unsigned *ids){
    static const unsigned fixed[]={19,20,21,22,24,26};size_t n=0;
    for(size_t i=0;i<sizeof fixed/sizeof fixed[0];++i)ids[n++]=fixed[i];
    unsigned allocated[BPP_MAX_ALLOCATABLE_REGS];size_t count=bpp_reg_saved_set(plan,allocated,BPP_MAX_ALLOCATABLE_REGS);
    for(size_t i=0;i<count;++i){int found=0;for(size_t j=0;j<n;++j)if(ids[j]==allocated[i])found=1;if(!found)ids[n++]=allocated[i];}
    for(size_t i=1;i<n;++i){unsigned x=ids[i];size_t j=i;while(j&&ids[j-1]>x){ids[j]=ids[j-1];--j;}ids[j]=x;}return n;
}
static size_t arm_save_area(const BPPRegPlan *plan){unsigned ids[BPP_MAX_ALLOCATABLE_REGS];size_t n=arm_saved_registers(plan,ids);BPPNativeStackUsage usage;if(!bpp_native_stack_usage(0,n,1,&usage))return SIZE_MAX;return usage.saved_bytes;}
static void arm_save_prologue(FILE *o,const BPPRegPlan *plan){
    unsigned ids[BPP_MAX_ALLOCATABLE_REGS];size_t n=arm_saved_registers(plan,ids),bytes=arm_save_area(plan);
    fputs("    stp x29, x30, [sp, #-16]!\n    .cfi_def_cfa_offset 16\n    .cfi_offset x29, -16\n    .cfi_offset x30, -8\n    mov x29, sp\n    .cfi_def_cfa x29, 16\n",o);
    if(bytes)fprintf(o,"    sub sp, sp, #%zu\n",bytes);
    for(size_t i=0;i<n;++i){size_t off=(i+1)*8;fprintf(o,"    str x%u, [x29, #-%zu]\n    .cfi_offset x%u, -%zu\n",ids[i],off,ids[i],16+off);}
}
static void arm_save_epilogue(FILE *o,const BPPRegPlan *plan){
    unsigned ids[BPP_MAX_ALLOCATABLE_REGS];size_t n=arm_saved_registers(plan,ids);
    fputs("    mov sp, x29\n",o);
    for(size_t i=0;i<n;++i){size_t off=(i+1)*8;fprintf(o,"    ldr x%u, [x29, #-%zu]\n",ids[i],off);}
    /* Keep the saved-register rules through the epilogue.  The save area is
     * still readable at CFA-relative offsets after the loads, and retaining
     * those rules avoids fragile CFI location advances across large native
     * blocks in Apple's assembler.  At ret, CFA is the caller's SP. */
    fputs("    ldp x29, x30, [sp], #16\n    .cfi_def_cfa sp, 0\n    ret\n    .cfi_endproc\n",o);
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
static void branch_block(Emitter *e,const char *instruction,size_t block) {
    char target[96];snprintf(target,sizeof target,"L_f%zu_b%zu",e->f,block);
    char inverse[64];
    if(!strncmp(instruction,"cbz ",4))snprintf(inverse,sizeof inverse,"cbnz %s",instruction+4);
    else if(!strncmp(instruction,"cbnz ",5))snprintf(inverse,sizeof inverse,"cbz %s",instruction+5);
    else snprintf(inverse,sizeof inverse,"%s",!strcmp(instruction,"b.ne")?"b.eq":!strcmp(instruction,"b.eq")?"b.ne":"b.ne");
    size_t near=e->near++;fprintf(e->out,"    %s L_near_%zu_%zu\n",inverse,e->f,near);farjump(e->out,target,0);fprintf(e->out,"L_near_%zu_%zu:\n",e->f,near);
}
static void define(Emitter *e,const char *name) {label(e,name);fputs(":\n",e->out);}
/* x19=stack value-frame base; globals and init bytes reside in the header. */
static void offset(Emitter *e,unsigned dst,unsigned base,size_t n) {
    BPPISelAddressRequest request={e->target,n,1};BPPISelAddressSelection selection;
    if(!bpp_isel_select_address(&request,&selection)) { imm(e->out,16,n);fprintf(e->out,"    add x%u, x%u, x16\n",dst,base);return; }
    if(selection.form==BPP_ISEL_ADDRESS_DIRECT){if(dst!=base)fprintf(e->out,"    mov x%u, x%u\n",dst,base);return;}
    if(selection.form==BPP_ISEL_ADDRESS_ARM64_ADD_IMM){uint64_t value=n>>selection.immediate_shift;fprintf(e->out,"    add x%u, x%u, #%llu%s\n",dst,base,(unsigned long long)value,selection.immediate_shift?", lsl #12":"");return;}
    imm(e->out,16,n);fprintf(e->out,"    add x%u, x%u, x16\n",dst,base);
}
static void regaddr(Emitter *e,unsigned dst,size_t r) { offset(e,dst,19,BPP_CG_HEADER_BYTES+e->plan->cell[r]*16); }
/* Construct the private frame directly. Parameter cloning remains a helper
 * because it changes observable object reference counts. */
static void frame_init(Emitter *e) {
    FILE *o=e->out;size_t flags=layout_flags(e->plan,e->fn),map_offset=64;
    char zero[64],done[64],layout_name[64];
    snprintf(zero,sizeof zero,"L_frame_zero_%zu",e->f);
    snprintf(done,sizeof done,"L_frame_ready_%zu",e->f);
    snprintf(layout_name,sizeof layout_name,"L_layout_%zu",e->f);
    fputs("    mov x17, x3\n    mov x9, x19\n",o);imm(o,10,e->layout.bytes);
    fprintf(o,"%s:\n    cbz x10, %s\n    str xzr, [x9], #8\n    sub x10, x10, #8\n    b %s\n%s:\n",zero,done,zero,done);
    fprintf(o,"    str x20, [x19, #%u]\n    str x21, [x19, #%u]\n    str x22, [x19, #%u]\n    str x24, [x19, #%u]\n",BPP_CG_FRAME_VM_OFFSET,BPP_CG_FRAME_GLOBALS_OFFSET,BPP_CG_FRAME_GLOBAL_INITIALIZED_OFFSET,BPP_CG_FRAME_OUTPUT_OFFSET);
    offset(e,9,19,BPP_CG_HEADER_BYTES);fprintf(o,"    str x9, [x19, #%u]\n",BPP_CG_FRAME_REGISTERS_OFFSET);
    offset(e,9,19,e->layout.locals);fprintf(o,"    str x9, [x19, #%u]\n",BPP_CG_FRAME_LOCALS_OFFSET);
    offset(e,9,19,e->layout.flags);fprintf(o,"    str x9, [x19, #%u]\n",BPP_CG_FRAME_LOCAL_INITIALIZED_OFFSET);
    offset(e,9,19,e->layout.args);fprintf(o,"    str x9, [x19, #%u]\n",BPP_CG_FRAME_ARGS_OFFSET);
    imm(o,9,e->fn->register_count);fprintf(o,"    str x9, [x19, #%u]\n",BPP_CG_FRAME_REGISTER_COUNT_OFFSET);
    imm(o,9,e->layout.local_slots);fprintf(o,"    str x9, [x19, #%u]\n",BPP_CG_FRAME_LOCAL_COUNT_OFFSET);
    imm(o,9,e->plan->cell_count);fprintf(o,"    str x9, [x19, #%u]\n",BPP_CG_FRAME_CELL_COUNT_OFFSET);
    imm(o,9,e->plan->root_cell_count);fprintf(o,"    str x9, [x19, #%u]\n",BPP_CG_ROOT_COUNT_OFFSET);
    if(flags&BPP_LAYOUT_REGISTER_MAP){addr(o,9,layout_name);offset(e,9,9,map_offset);fprintf(o,"    str x9, [x19, #%u]\n",BPP_CG_FRAME_REGISTER_MAP_OFFSET);map_offset+=e->fn->register_count*8;}
    if(flags&BPP_LAYOUT_LOCAL_MAP){addr(o,9,layout_name);offset(e,9,9,map_offset);fprintf(o,"    str x9, [x19, #%u]\n",BPP_CG_FRAME_LOCAL_MAP_OFFSET);}
    for(size_t parameter=0;parameter<e->fn->param_count;++parameter){
        size_t source=parameter*BPP_CG_VALUE_BYTES;
        size_t physical=e->plan->local_map?e->plan->local_map[parameter]:parameter;
        size_t destination=e->layout.locals+physical*BPP_CG_VALUE_BYTES;
        size_t initialized=e->layout.flags+physical;
        offset(e,12,17,source);fputs("    ldr w9, [x12]\n    ldr x10, [x12, #8]\n",o);
        offset(e,12,19,destination);fputs("    str w9, [x12]\n    str x10, [x12, #8]\n",o);offset(e,12,19,initialized);fputs("    mov w9, #1\n    strb w9, [x12]\n",o);offset(e,12,19,destination);fputs("    ldr w9, [x12]\n    cmp w9, #2\n",o);
        fprintf(o,"    b.ne L_param_no_retain_%zu_%zu\n    cbz x10, L_param_no_retain_%zu_%zu\n    ldr w9, [x10, #%u]\n    add w9, w9, #1\n    str w9, [x10, #%u]\nL_param_no_retain_%zu_%zu:\n",e->f,parameter,e->f,parameter,BPP_CG_OBJECT_REF_COUNT_OFFSET,BPP_CG_OBJECT_REF_COUNT_OFFSET,e->f,parameter);
    }
}
static void cleanup_cell(Emitter *e,size_t displacement,const char *area,size_t index){
    offset(e,12,19,displacement);
    fprintf(e->out,"    ldr w9, [x12]\n    cmp w9, #2\n    b.ne L_cleanup_%s_skip_%zu_%zu\n    ldr x0, [x12, #8]\n    cbz x0, L_cleanup_%s_skip_%zu_%zu\n    bl _bpp_%s_release_object\nL_cleanup_%s_skip_%zu_%zu:\n",area,e->f,index,area,e->f,index,e->native_release?"n":"a",area,e->f,index);
    offset(e,12,19,displacement);
    fputs("    str wzr, [x12]\n    str xzr, [x12, #8]\n",e->out);
}
static void cleanup_frame(Emitter *e){
    for(size_t root=0;root<e->plan->root_cell_count;++root)
        cleanup_cell(e,BPP_CG_HEADER_BYTES+root*BPP_CG_VALUE_BYTES,"root",root);
    for(size_t local=0;local<e->plan->local_slot_count;++local)
        cleanup_cell(e,e->layout.locals+local*BPP_CG_VALUE_BYTES,"local",local);
}
/* Outgoing arguments are borrowed BLValue pairs.  The slow path has already
 * synchronized the source cell, so copying tag/payload requires no runtime
 * helper or reference-count operation. */
static void copy_arg(Emitter *e,size_t arg,size_t source) {
    regaddr(e,12,source);
    fputs("    ldr w9, [x12]\n    ldr x10, [x12, #8]\n",e->out);
    fprintf(e->out,"    ldr x12, [x19, #%u]\n",BPP_CG_FRAME_ARGS_OFFSET);
    offset(e,12,12,arg*BPP_CG_VALUE_BYTES);
    fputs("    str w9, [x12]\n    str x10, [x12, #8]\n",e->out);
}
static size_t position(Emitter *e){return bpp_reg_position(e->plan,e->b,e->k);}
static unsigned physical_at(Emitter *e,size_t r,size_t pos){BPPRegLocation loc=bpp_reg_location_at(e->plan,r,pos);return loc.kind==BPP_LOC_REGISTER?loc.physical:0;}
static void spill_note(Emitter *e,size_t r,BPPSpillReason reason,unsigned physical){if(e->asm_comments){const BPPRegisterInfo *ri=bpp_target_register(BPP_MACOS_ARM64,physical);fprintf(e->out,"    // B++ spill vreg=v%zu physical=%s cell=%zu reason=%s\n",r,ri?ri->name:"?",e->plan->cell[r],bpp_reg_spill_reason_name(reason));}}
static const BLIRInst *frame_site(const BLIRFunction *fn){const BLIRInst *first=NULL,*largest=NULL;size_t maxargs=0;for(size_t b=0;b<fn->block_count;++b)for(size_t k=0;k<fn->blocks[b].count;++k){const BLIRInst *i=&fn->blocks[b].insts[k];if(!first)first=i;if(i->arg_count>maxargs){maxargs=i->arg_count;largest=i;}}return largest?largest:first;}
static void frame_error(BLIRModule *m,const BLIRFunction *fn){const BLIRInst *site=frame_site(fn);snprintf(m->error,sizeof m->error,"%s:%d:%d: ARM64 frame layout overflow in function '%s'",m->path?m->path:"<input>",site?site->line:1,site?site->column:1,fn->name);}
static void payload(Emitter *e,unsigned dst,size_t r) {
    BPPRegLocation location=bpp_reg_location_at(e->plan,r,position(e));
    if(location.kind==BPP_LOC_REMATERIALIZABLE){if(e->asm_comments)fprintf(e->out,"    // B++ rematerialize vreg=v%zu immediate=%lld reason=safe-integer-expression\n",r,location.immediate);imm(e->out,dst,(uint64_t)location.immediate);++e->emitted_rematerializations;return;}
    unsigned physical=location.kind==BPP_LOC_REGISTER?location.physical:0;
    if(physical){if(e->dirty[r]&&e->dirty_phys[r]&&e->dirty_phys[r]!=physical){unsigned old=e->dirty_phys[r];if(e->asm_comments){const BPPRegisterInfo *ri=bpp_target_register(BPP_MACOS_ARM64,old);fprintf(e->out,"    // B++ spill vreg=v%zu physical=%s cell=%zu reason=allocation-segment-transition\n",r,ri?ri->name:"?",e->plan->cell[r]);}regaddr(e,12,r);fprintf(e->out,"    str x%u, [x12, #8]\n",old);++e->emitted_spills;e->dirty[r]=0;e->dirty_phys[r]=0;e->loaded_phys[r]=0;}if(e->loaded_phys[r]!=physical){if(e->asm_comments){const BPPRegisterInfo *ri=bpp_target_register(BPP_MACOS_ARM64,physical);fprintf(e->out,"    // B++ reload vreg=v%zu physical=%s cell=%zu reason=%s\n",r,ri?ri->name:"?",e->plan->cell[r],e->loaded_phys[r]?"allocation-segment-change":"first-payload-use");}regaddr(e,12,r);fprintf(e->out,"    ldr x%u, [x12, #8]\n",physical);++e->emitted_reloads;e->loaded_phys[r]=physical;}fprintf(e->out,"    mov x%u, x%u\n",dst,physical);}
    else {regaddr(e,12,r);fprintf(e->out,"    ldr x%u, [x12, #8]\n",dst);}
}
/* Dirty payloads remain in callee-saved registers until a consumer needs cells.
 * Conditional slow paths flush the pre-instruction snapshot without changing
 * the conservative state at the fast/slow merge. */
static void flush(Emitter *e,unsigned char *dirty,int clear) {
    for(size_t r=0;r<e->fn->register_count;++r) if(dirty[r]) {
        BPPSpillReason reason=bpp_reg_spill_reason_before(e->plan,e->b,e->k,r);unsigned physical=e->dirty_phys[r];if(reason!=BPP_SPILL_NONE&&physical){spill_note(e,r,reason,physical);regaddr(e,12,r);fprintf(e->out,"    str x%u, [x12, #8]\n",physical);++e->emitted_spills;}
        if(clear){dirty[r]=0;e->dirty_phys[r]=0;}
    }
}
static void edge_flush(Emitter *e,size_t to) {
    for(size_t r=0;r<e->fn->register_count;++r)if(e->dirty[r]){
        unsigned physical=e->dirty_phys[r];if(!e->instrument_native&&physical&&bpp_reg_keep_dirty_on_edge(e->plan,e->b,to,r,physical))continue;
        BPPSpillReason reason=bpp_reg_spill_reason_on_edge(e->plan,e->b,to,r);if(reason!=BPP_SPILL_NONE&&physical){spill_note(e,r,reason,physical);regaddr(e,12,r);fprintf(e->out,"    str x%u, [x12, #8]\n",physical);++e->emitted_spills;}
    }
}
static void edge_flush_pair(Emitter *e,size_t first,size_t second){edge_flush(e,first);edge_flush(e,second);}
/* The selected next block needs no unconditional transfer.  This uses the
 * inverse conditional form where necessary; edge homes are made visible for
 * both outcomes before the split because neither path has an edge stub. */
static int branch_fallthrough(Emitter *e,const BLIRInst *i) {
    if(e->b+1>=e->fn->block_count||(i->b!=e->b+1&&i->c!=e->b+1))return 0;
    edge_flush_pair(e,i->b,i->c);regaddr(e,12,i->a);fputs("    ldr w9, [x12]\n",e->out);
    branch_block(e,"cbz w9,",i->c);fputs("    cmp w9, #1\n",e->out);branch_block(e,"b.ne",i->b);payload(e,9,i->a);
    fputs("    cmp x9, #0\n",e->out);if(i->b==e->b+1)branch_block(e,"b.le",i->c);else branch_block(e,"b.gt",i->b);
    if(e->asm_comments)fputs("    // B++ branch-layout inverted conditional; selected successor falls through\n",e->out);
    return 1;
}
static void cache_result(Emitter *e,size_t r,unsigned src) {
    unsigned physical=physical_at(e,r,position(e));
    if(physical) {
        /* Preserve an old dirty owner before this physical register is reused. */
        for(size_t a=0;a<e->fn->register_count;++a)
            if(a!=r && (physical_at(e,a,position(e))==physical || e->dirty_phys[a]==physical)) {
                if(e->dirty[a]&&e->dirty_phys[a]==physical){BPPSpillReason reason=bpp_reg_spill_reason_before(e->plan,e->b,e->k,a);if(reason!=BPP_SPILL_NONE){spill_note(e,a,reason,physical);regaddr(e,12,a);fprintf(e->out,"    str x%u, [x12, #8]\n",e->dirty_phys[a]);++e->emitted_spills;}e->dirty[a]=0;e->dirty_phys[a]=0;}
                if(e->loaded_phys[a]==physical)e->loaded_phys[a]=0;
            }
        if(src==31) fprintf(e->out,"    mov x%u, xzr\n",physical); else fprintf(e->out,"    mov x%u, x%u\n",physical,src);e->loaded_phys[r]=physical; }
}
static void restore_live_after(Emitter *e){
    size_t old=position(e),pos=old+1;const BLIRInst *i=&e->fn->blocks[e->b].insts[e->k];size_t dst=i->op==BL_IR_RELEASE?i->a:i->dst;
    for(size_t r=0;r<e->fn->register_count;++r)if(bpp_reg_live_after(e->plan,e->b,e->k,r)){
        unsigned before=physical_at(e,r,old),after=physical_at(e,r,pos);
        if(after&&(r==dst||after!=before))e->loaded_phys[r]=0;
    }
}
static BPPSpillReason slow_edge_reason(Emitter *e,size_t r){
    size_t pos=position(e);if(e->fn->blocks[e->b].insts[e->k].dst==r)return BPP_SPILL_DESTINATION;
    if(bpp_reg_instruction_visible(e->plan,e->b,e->k,r))return BPP_SPILL_HELPER_INPUT;
    if(e->plan->alloc_class[r]==BPP_ALLOC_TAGGED_PAIR&&e->plan->begin[r]!=SIZE_MAX&&e->plan->begin[r]<=pos&&e->plan->end[r]>=pos)return BPP_SPILL_ERROR_CLEANUP;
    if(bpp_reg_live_after(e->plan,e->b,e->k,r))return BPP_SPILL_LIVE_ACROSS;
    return bpp_reg_spill_reason_before(e->plan,e->b,e->k,r);
}
static void sync_slow_edge_homes(Emitter *e){size_t pos=position(e);for(size_t r=0;r<e->fn->register_count;++r)if(e->dirty[r]){BPPSpillReason reason=slow_edge_reason(e,r);unsigned physical=e->dirty_phys[r];if(reason==BPP_SPILL_LIVE_ACROSS&&physical&&physical_at(e,r,pos+1)==physical)continue;if(reason!=BPP_SPILL_NONE&&physical){spill_note(e,r,reason,physical);regaddr(e,12,r);fprintf(e->out,"    str x%u, [x12, #8]\n",physical);++e->emitted_spills;}e->dirty[r]=0;e->dirty_phys[r]=0;}}
static void intguard(Emitter *e,size_t r) {if(e->kind[r]==BPP_KIND_INT)return;regaddr(e,12,r);fputs("    ldr w11, [x12]\n    cmp w11, #1\n",e->out);branch(e,"b.ne","slow");}
/* This is the native equivalent of bpp_a_destination: release the prior
 * object, if any, and make the canonical cell null. x26 is callee-saved and
 * therefore retains a primitive result across bpp_n_release_object; x12 is
 * recovered from the frame scratch slot after that call. */
static void native_destination(Emitter *e,size_t dst){
    size_t id=e->near++;
    regaddr(e,12,dst);fprintf(e->out,"    str x12, [x19, #%u]\n    ldr w11, [x12]\n    cmp w11, #2\n    b.ne L_native_destination_skip_%zu_%zu_%zu_%zu\n    ldr x0, [x12, #8]\n    cbz x0, L_native_destination_skip_%zu_%zu_%zu_%zu\n    bl _bpp_n_release_object\nL_native_destination_skip_%zu_%zu_%zu_%zu:\n    ldr x12, [x19, #%u]\n    str wzr, [x12]\n    str xzr, [x12, #8]\n",BPP_CG_CALL_OUTPUT_OFFSET,e->f,e->b,e->k,id,e->f,e->b,e->k,id,e->f,e->b,e->k,id,BPP_CG_CALL_OUTPUT_OFFSET);
}
/* Native BLString construction for executable output. */
static void native_string(Emitter *e,const BLIRInst *i){
    const char *text=i->text?i->text:"";size_t length=strlen(text),id=e->near++;
    char label[96];snprintf(label,sizeof label,"L_text_%zu_%zu_%zu",e->f,e->b,e->k);
    native_destination(e,i->dst);
    imm(e->out,0,1);imm(e->out,1,BPP_CG_STRING_BYTES);fputs("    bl _bpp_n_allocate_zero\n",e->out);fprintf(e->out,"    cbnz x0, L_native_string_object_%zu_%zu_%zu_%zu\n    bl _bpp_n_allocation_failed\nL_native_string_object_%zu_%zu_%zu_%zu:\n    str x0, [x19, #%u]\n",e->f,e->b,e->k,id,e->f,e->b,e->k,id,BPP_CG_CALL_OUTPUT_OFFSET);
    imm(e->out,0,length+1);imm(e->out,1,1);fputs("    bl _bpp_n_allocate_zero\n",e->out);fprintf(e->out,"    cbnz x0, L_native_string_data_%zu_%zu_%zu_%zu\n    bl _bpp_n_allocation_failed\nL_native_string_data_%zu_%zu_%zu_%zu:\n",e->f,e->b,e->k,id,e->f,e->b,e->k,id);
    addr(e->out,1,label);imm(e->out,2,length+1);fputs("    bl _memcpy\n",e->out);
    fprintf(e->out,"    ldr x10, [x19, #%u]\n    mov w11, #1\n    str w11, [x10]\n    str w11, [x10, #4]\n    str x0, [x10, #8]\n",BPP_CG_CALL_OUTPUT_OFFSET);imm(e->out,11,length);fputs("    str x11, [x10, #16]\n",e->out);regaddr(e,12,i->dst);fputs("    mov w11, #2\n    str w11, [x12]\n    str x10, [x12, #8]\n",e->out);
}
/* x26 retains the primitive result across an optional object-release call. */
static void result(Emitter *e,size_t dst,unsigned tag) {
    regaddr(e,12,dst);fputs("    ldr w11, [x12]\n    cmp w11, #2\n",e->out);branch(e,"b.ne","write");
    flush(e,e->dirty,0);
    if(e->native_release){native_destination(e,dst);restore_live_after(e);}else{fputs("    mov x0, x19\n",e->out);imm(e->out,1,dst);fputs("    bl _bpp_a_destination\n    mov x17, x0\n",e->out);restore_live_after(e);fputs("    mov x12, x17\n",e->out);}
    define(e,"write");fprintf(e->out,"    mov w11, #%u\n    str w11, [x12]\n",tag);
    if(physical_at(e,dst,position(e))) {
        cache_result(e,dst,26);const BLIRInst *current=&e->fn->blocks[e->b].insts[e->k];int helper_join=current->op!=BL_IR_INT&&current->op!=BL_IR_NULL;
        if(tag==1&&helper_join){fputs("    str x26, [x12, #8]\n",e->out);e->dirty[dst]=0;e->dirty_phys[dst]=0;}
        else{e->dirty[dst]=(unsigned char)(tag==1);e->dirty_phys[dst]=tag==1?physical_at(e,dst,position(e)):0;}
    } else if(tag==1) fputs("    str x26, [x12, #8]\n",e->out);
    branch(e,"b","done");
}
static size_t local_slot(Emitter *e,size_t local){return e->plan->local_map?e->plan->local_map[local]:local;}
static void slotaddr(Emitter *e,unsigned dst,int global,size_t slot) {if(global)fputs("    ldr x12, [x19, #8]\n",e->out);else slot=local_slot(e,slot);offset(e,dst,global?12:19,(global?0:e->layout.locals)+slot*16);}
static void flagaddr(Emitter *e,unsigned dst,int global,size_t slot) {if(global)fputs("    ldr x12, [x19, #40]\n",e->out);else slot=local_slot(e,slot);offset(e,dst,global?12:19,(global?0:e->layout.flags)+slot);}
static int fast(Emitter *e,const BLIRInst *i) {
    FILE *o=e->out;
    if(e->truth_states&&(i->op==BL_IR_MALLOC||i->op==BL_IR_REMALLOC||i->op==BL_IR_MFREE_LOCAL||i->op==BL_IR_MFREE_GLOBAL||i->op==BL_IR_BYTE_LOAD||i->op==BL_IR_BYTE_STORE||i->op==BL_IR_PRINT_BYTE||i->op==BL_IR_PRINT_RANGE))return 0;
    switch(i->op) {
    case BL_IR_NULL: imm(o,26,0);result(e,i->dst,0);return 1;
    case BL_IR_INT: imm(o,26,(uint64_t)i->integer);result(e,i->dst,1);return 1;
    case BL_IR_MALLOC: {
        const char *p=e->target==BPP_MACOS_ARM64?"_":"";intguard(e,i->a);payload(e,0,i->a);fprintf(o,"    bl %smalloc\n",p);fputs("    mov x26, x0\n",o);result(e,i->dst,1);return 1; }
    case BL_IR_REMALLOC: {
        const char *p=e->target==BPP_MACOS_ARM64?"_":"";intguard(e,i->a);intguard(e,i->b);payload(e,0,i->a);payload(e,1,i->b);fprintf(o,"    bl %srealloc\n",p);fputs("    mov x26, x0\n",o);result(e,i->dst,1);return 1; }
    case BL_IR_BYTE_LOAD:
        intguard(e,i->a);intguard(e,i->b);payload(e,9,i->a);payload(e,10,i->b);fputs("    ldrb w26, [x9, x10]\n",o);result(e,i->dst,1);return 1;
    case BL_IR_BYTE_STORE:
        intguard(e,i->a);intguard(e,i->b);intguard(e,i->c);payload(e,9,i->a);payload(e,10,i->b);payload(e,11,i->c);fputs("    strb w11, [x9, x10]\n",o);branch(e,"b","done");return 1;
    case BL_IR_MFREE_LOCAL: case BL_IR_MFREE_GLOBAL: {
        const char *p=e->target==BPP_MACOS_ARM64?"_":"";int g=i->op==BL_IR_MFREE_GLOBAL;slotaddr(e,12,g,i->a);fputs("    ldr x0, [x12, #8]\n",o);fprintf(o,"    bl %sfree\n",p);branch(e,"b","done");return 1; }
    case BL_IR_PRINT_BYTE: {
        const char *p=e->target==BPP_MACOS_ARM64?"_":"";intguard(e,i->a);payload(e,9,i->a);fputs("    strb w9, [x19, #104]\n    mov x0, #1\n    add x1, x19, #104\n    mov x2, #1\n",o);fprintf(o,"    bl %swrite\n",p);branch(e,"b","done");return 1; }
    case BL_IR_PRINT_RANGE: {
        const char *p=e->target==BPP_MACOS_ARM64?"_":"";intguard(e,i->a);intguard(e,i->b);payload(e,1,i->a);payload(e,2,i->b);fputs("    mov x0, #1\n",o);fprintf(o,"    bl %swrite\n",p);branch(e,"b","done");return 1; }
    case BL_IR_RELEASE:
        regaddr(e,12,i->a);fputs("    ldr w11, [x12]\n    cmp w11, #2\n",o);branch(e,"b.eq","slow");
        fputs("    str wzr, [x12]\n",o);e->dirty[i->a]=0;e->dirty_phys[i->a]=0;cache_result(e,i->a,31);branch(e,"b","done");return 1;
    case BL_IR_LOAD_LOCAL: case BL_IR_LOAD_GLOBAL: {
        int g=i->op==BL_IR_LOAD_GLOBAL;flagaddr(e,12,g,i->a);fputs("    ldrb w11, [x12]\n",o);branch(e,"cbz w11,","slow");
        slotaddr(e,12,g,i->a);fputs("    ldr w11, [x12]\n    cmp w11, #1\n",o);branch(e,"b.ne","slow");fputs("    ldr x26, [x12, #8]\n",o);result(e,i->dst,1);return 1;
    }
    case BL_IR_INIT_LOCAL: case BL_IR_INIT_GLOBAL: case BL_IR_STORE_LOCAL: case BL_IR_STORE_GLOBAL: {
        int g=i->op==BL_IR_INIT_GLOBAL||i->op==BL_IR_STORE_GLOBAL;
        int init=i->op==BL_IR_INIT_GLOBAL||i->op==BL_IR_INIT_LOCAL;
        if(!g&&i->op==BL_IR_INIT_LOCAL&&e->plan->local_slot_shared[local_slot(e,i->a)])return 0;
        intguard(e,i->b);
        if(!init){flagaddr(e,12,g,i->a);fputs("    ldrb w11, [x12]\n",o);branch(e,"cbz w11,","slow");}
        slotaddr(e,12,g,i->a);fputs("    ldr w11, [x12]\n    cmp w11, #2\n",o);branch(e,"b.eq","slow");
        payload(e,26,i->b);slotaddr(e,12,g,i->a);fputs("    mov w11, #1\n    str w11, [x12]\n    str x26, [x12, #8]\n",o);
        if(init){flagaddr(e,12,g,i->a);fputs("    mov w11, #1\n    strb w11, [x12]\n",o);}
        branch(e,"b","done");return 1;
    }
    case BL_IR_BIT_NOT:
        intguard(e,i->a);payload(e,9,i->a);fputs("    mvn x26, x9\n",o);result(e,i->dst,1);return 1;
    case BL_IR_BIT_AND:case BL_IR_BIT_OR:case BL_IR_BIT_XOR:
        intguard(e,i->a);intguard(e,i->b);payload(e,9,i->a);payload(e,10,i->b);
        fprintf(o,"    %s x26, x9, x10\n",i->op==BL_IR_BIT_AND?"and":i->op==BL_IR_BIT_OR?"orr":"eor");result(e,i->dst,1);return 1;
    case BL_IR_NOT:return 0;
    case BL_IR_NEG:
        intguard(e,i->a);payload(e,9,i->a);fputs("    negs x26, x9\n",o);branch(e,"b.vs","slow");result(e,i->dst,1);return 1;
    case BL_IR_ADD: case BL_IR_SUB: case BL_IR_MUL: case BL_IR_DIV: case BL_IR_MOD:
    case BL_IR_EQ: case BL_IR_NE: case BL_IR_LT: case BL_IR_LE: case BL_IR_GT: case BL_IR_GE:
        {BPPISelRequest request={e->target,i->op,e->constant[i->b],e->value[i->b]};BPPInstructionSelection selection;
        if(!bpp_isel_select(&request,&selection)||selection.scratch_gprs>4)return 0;
        if(e->asm_comments)fprintf(o,"    // B++ isel form=%s cost=%u scratch-gprs=%u flags=%s\n",bpp_isel_form_name(selection.form),selection.cost,selection.scratch_gprs,selection.defines_flags?"defined-consumed-locally":"none");
        intguard(e,i->a);intguard(e,i->b);payload(e,9,i->a);
        if(selection.form==BPP_ISEL_ARM64_ADD_SUB_IMM){
            uint64_t mag=selection.immediate<0?(uint64_t)(-(selection.immediate+1))+1:(uint64_t)selection.immediate;
            int subtract=(i->op==BL_IR_SUB)^(selection.immediate<0);
            fprintf(o,"    %s x26, x9, #%llu%s\n",subtract?"subs":"adds",(unsigned long long)(mag>>selection.immediate_shift),selection.immediate_shift?", lsl #12":"");branch(e,"b.vs","slow");result(e,i->dst,1);return 1;
        }
        if(selection.form==BPP_ISEL_ARM64_CMP_IMM){
            uint64_t mag=selection.immediate<0?(uint64_t)(-(selection.immediate+1))+1:(uint64_t)selection.immediate;
            fprintf(o,"    %s x9, #%llu%s\n",selection.immediate<0?"cmn":"cmp",(unsigned long long)(mag>>selection.immediate_shift),selection.immediate_shift?", lsl #12":"");
            const char *condition=i->op==BL_IR_EQ?"eq":i->op==BL_IR_NE?"ne":i->op==BL_IR_LT?"lt":i->op==BL_IR_LE?"le":i->op==BL_IR_GT?"gt":"ge";
            fprintf(o,"    cset x26, %s\n    lsl x26, x26, #1\n    sub x26, x26, #1\n",condition);result(e,i->dst,1);return 1;
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
            fprintf(o,"    cmp x9, x10\n    cset x26, %s\n    lsl x26, x26, #1\n    sub x26, x26, #1\n",condition);break;
        }
        }
        result(e,i->dst,1);return 1;}
    default:return 0;
    }
}
static void slow(Emitter *e,const BLIRInst *i,int conditional) {
    flush(e,conditional?e->before:e->dirty,!conditional);
    FILE *o=e->out;fputs("    mov x0, x19\n",o);
    if(i->op==BL_IR_STRING){if(e->native_release)native_string(e,i);else{imm(o,1,i->dst);char s[96];snprintf(s,sizeof s,"L_text_%zu_%zu_%zu",e->f,e->b,e->k);addr(o,2,s);fputs("    bl _bpp_a_string\n",o);}}
    else if(i->op==BL_IR_CALL) {
        for(size_t j=0;j<i->arg_count;++j)copy_arg(e,j,i->args[j]);
        fputs("    mov x0, x19\n",o);
        if(i->a==BL_IR_NONE&&i->c==BL_IR_NONE){imm(o,1,i->dst);char s[96];snprintf(s,sizeof s,"L_text_%zu_%zu_%zu",e->f,e->b,e->k);addr(o,2,s);imm(o,3,i->arg_count);imm(o,4,((uint64_t)(i->source_file==BL_IR_NONE?0:i->source_file+1)<<32)|(uint32_t)(i->source_line>0?i->source_line:i->line));imm(o,5,(uint64_t)(i->source_column>0?i->source_column:i->column));fputs("    bl _bpp_a_builtin\n",o);}
        else{if(e->truth_states){fputs("    mov x0, x19\n    bl _bpp_a_call_begin\n    mov x0, x19\n",o);}if(e->native_release)native_destination(e,i->dst);else{imm(o,1,i->dst);fputs("    bl _bpp_a_destination\n",o);fprintf(o,"    str x0, [x19, #%u]\n",BPP_CG_CALL_OUTPUT_OFFSET);}fprintf(o,"    ldr x3, [x19, #%u]\n    ldr x0, [x19, #%u]\n    ldr x1, [x19, #%u]\n    ldr x2, [x19, #%u]\n",BPP_CG_FRAME_ARGS_OFFSET,BPP_CG_FRAME_VM_OFFSET,BPP_CG_FRAME_GLOBALS_OFFSET,BPP_CG_FRAME_GLOBAL_INITIALIZED_OFFSET);fprintf(o,"    ldr x4, [x19, #%u]\n",BPP_CG_CALL_OUTPUT_OFFSET);
            char target[128];
            if(i->c!=BL_IR_NONE)snprintf(target,sizeof target,e->target==BPP_MACOS_ARM64?"_%s":"%s",i->text);
            else snprintf(target,sizeof target,"L_proc_%zu",i->a);
            farjump(o,target,1);if(e->truth_states){fputs("    mov x0, x19\n",o);imm(o,1,i->dst);fputs("    bl _bpp_a_call_end\n",o);}}
    } else{imm(o,1,i->op);imm(o,2,i->dst);imm(o,3,i->a);imm(o,4,i->b);imm(o,5,i->c);imm(o,6,((uint64_t)(i->source_file==BL_IR_NONE?0:i->source_file+1)<<32)|(uint32_t)(i->source_line>0?i->source_line:i->line));imm(o,7,(uint64_t)(i->source_column>0?i->source_column:i->column));fputs("    bl _bpp_a_op\n",o);}
    fprintf(o,"    ldr x12, [x19, #%u]\n    ldr x12, [x12, #%u]\n",BPP_CG_FRAME_VM_OFFSET,BPP_CG_RUNTIME_ERROR_OFFSET);branch(e,"cbz x12,","post_call");char failed[64];snprintf(failed,sizeof failed,"L_fail_%zu",e->f);farjump(o,failed,0);
    define(e,"post_call");
    restore_live_after(e);
    /* Both paths leave the destination home current. Let its next native use
     * perform the reload instead of reloading it at this join. */
    if(i->dst!=BL_IR_NONE){e->loaded_phys[i->dst]=0;e->dirty[i->dst]=0;e->dirty_phys[i->dst]=0;}
    branch(e,"b","done");
}
/* Native executable launcher; C leaves are limited to allocation, diagnostics,
 * result formatting, and refcount destruction. */
/* Same private platform boundary as the x86-64 emitter: the generated image
 * owns these leaves and only tail-calls the OS C ABI allocator/process entry.
 * They are deliberately separate from the B++ dynamic-value runtime. */
static void emit_native_platform_leaves(FILE *out,BPPTarget target){
    const char *prefix=target==BPP_MACOS_ARM64?"_":"";
    fprintf(out,".p2align 2\n.globl _bpp_n_allocate_zero\n_bpp_n_allocate_zero:\n    b %scalloc\n",prefix);
    fprintf(out,".p2align 2\n.globl _bpp_n_free_memory\n_bpp_n_free_memory:\n    b %sfree\n",prefix);
    fprintf(out,".p2align 2\n.globl _bpp_n_allocation_failed\n_bpp_n_allocation_failed:\n    mov w0, #1\n    b %sexit\n",prefix);
    /* BLString is { header, data@8, size@16 }; buffer/array add owned@24.
     * Keep this leaf in lockstep with the private facts in codegen_abi.h. */
    fputs(".p2align 2\n.globl _bpp_n_release_object\n_bpp_n_release_object:\n    cbz x0, L_native_release_ret\n    stp x29, x30, [sp, #-16]!\n    mov x29, sp\n    stp x19, x20, [sp, #-16]!\n    stp x21, x22, [sp, #-16]!\n    mov x19, x0\n    ldr w9, [x19, #4]\n    subs w9, w9, #1\n    str w9, [x19, #4]\n    b.ne L_native_release_done\n    ldr w9, [x19]\n    cmp w9, #1\n    b.eq L_native_release_string\n    cmp w9, #2\n    b.eq L_native_release_buffer\n    cmp w9, #4\n    b.eq L_native_release_object\n    cmp w9, #3\n    b.eq L_native_release_array\n    b L_native_release_done\nL_native_release_string:\n    ldr x0, [x19, #8]\n    cbz x0, L_native_release_object\n    bl _bpp_n_free_memory\nL_native_release_object:\n    mov x0, x19\n    bl _bpp_n_free_memory\n    b L_native_release_done\nL_native_release_buffer:\n    ldr w9, [x19, #24]\n    cbz w9, L_native_release_object\n    ldr x0, [x19, #8]\n    cbz x0, L_native_release_object\n    bl _bpp_n_free_memory\n    b L_native_release_object\nL_native_release_array:\n    ldr w9, [x19, #24]\n    cbz w9, L_native_release_object\n    ldr x20, [x19, #8]\n    cbz x20, L_native_release_object\n    ldr x21, [x19, #16]\n    lsl x21, x21, #4\n    add x21, x20, x21\nL_native_release_array_loop:\n    cmp x20, x21\n    b.eq L_native_release_array_items\n    ldr w9, [x20]\n    cmp w9, #2\n    b.ne L_native_release_array_next\n    ldr x0, [x20, #8]\n    cbz x0, L_native_release_array_next\n    bl _bpp_n_release_object\nL_native_release_array_next:\n    add x20, x20, #16\n    b L_native_release_array_loop\nL_native_release_array_items:\n    ldr x0, [x19, #8]\n    bl _bpp_n_free_memory\n    b L_native_release_object\nL_native_release_done:\n    ldp x21, x22, [sp], #16\n    ldp x19, x20, [sp], #16\n    ldp x29, x30, [sp], #16\nL_native_release_ret:\n    ret\n",out);
    /* Darwin AArch64 puts variadic arguments in a different ABI area, so an
     * otherwise innocent printf("%lld", value) is not portable here. Format
     * integer results directly and use the non-variadic write ABI. Objects
     * retain the C formatter until their native representation is complete. */
    fprintf(out,".p2align 2\n.globl _bpp_n_print_result\n_bpp_n_print_result:\n    cbz x0, L_native_print_ret\n    stp x29, x30, [sp, #-16]!\n    mov x29, sp\n    sub sp, sp, #64\n    mov x9, x0\n    ldr w10, [x9]\n    cbz w10, L_native_print_done\n    cmp w10, #1\n    b.eq L_native_print_int\n    cmp w10, #2\n    b.ne L_native_print_done\n    mov x0, x9\n    bl _bpp_a_print_result\n    b L_native_print_done\nL_native_print_int:\n    ldr x10, [x9, #8]\n    add x13, sp, #64\n    sub x13, x13, #1\n    mov w11, #10\n    mov w12, #10\n    strb w12, [x13]\n    mov w12, #0\n    cmp x10, #0\n    b.ge L_native_print_magnitude\n    mov w12, #1\n    neg x10, x10\nL_native_print_magnitude:\n    cbnz x10, L_native_print_digits\n    sub x13, x13, #1\n    mov w14, #48\n    strb w14, [x13]\n    b L_native_print_sign\nL_native_print_digits:\n    udiv x14, x10, x11\n    msub x15, x14, x11, x10\n    sub x13, x13, #1\n    add x15, x15, #48\n    strb w15, [x13]\n    mov x10, x14\n    cbnz x10, L_native_print_digits\nL_native_print_sign:\n    cbz w12, L_native_print_write\n    sub x13, x13, #1\n    mov w14, #45\n    strb w14, [x13]\nL_native_print_write:\n    add x2, sp, #64\n    sub x2, x2, x13\n    mov x1, x13\n    mov x0, #1\n    bl %swrite\nL_native_print_done:\n    add sp, sp, #64\n    ldp x29, x30, [sp], #16\nL_native_print_ret:\n    ret\n",prefix);
    fprintf(out,".p2align 2\n.globl _bpp_n_report_error\n_bpp_n_report_error:\n    cbz x0, L_native_report_ret\n    stp x29, x30, [sp, #-16]!\n    mov x29, sp\n    stp x19, x20, [sp, #-16]!\n    mov x19, x0\n    bl %sstrlen\n    mov x2, x0\n    mov x1, x19\n    mov x0, #2\n    bl %swrite\n    adrp x1, L_native_newline@PAGE\n    add x1, x1, L_native_newline@PAGEOFF\n    mov x2, #1\n    mov x0, #2\n    bl %swrite\n    ldp x19, x20, [sp], #16\n    ldp x29, x30, [sp], #16\nL_native_report_ret:\n    ret\n",prefix,prefix,prefix);
}
static void main_state(Emitter *e,unsigned reg){fprintf(e->out,"    sub x%u, x29, #64\n",reg);}
static void main_release_value(Emitter *e,int global,size_t index){
    const char *area=global?"global":"result";
    main_state(e,9);
    if(global){fputs("    ldr x10, [x9, #16]\n",e->out);if(index)offset(e,10,10,index*BPP_CG_VALUE_BYTES);}
    else fputs("    add x10, x9, #32\n",e->out);
    fprintf(e->out,"    ldr w11, [x10]\n    cmp w11, #2\n    b.ne L_main_%s_skip_%zu\n    ldr x0, [x10, #8]\n    cbz x0, L_main_%s_skip_%zu\n    bl _bpp_%s_release_object\nL_main_%s_skip_%zu:\n",area,index,area,index,e->truth_states?"a":"n",area,index);
    main_state(e,9);
    if(global){fputs("    ldr x10, [x9, #16]\n",e->out);if(index)offset(e,10,10,index*BPP_CG_VALUE_BYTES);}
    else fputs("    add x10, x9, #32\n",e->out);
    fputs("    str wzr, [x10]\n    str xzr, [x10, #8]\n",e->out);
}
static void emit_native_main(Emitter *e,size_t global_count,int print){
    size_t globals=global_count?global_count:1;
    fputs(".globl _main\n_main:\n    .cfi_startproc\n    stp x29, x30, [sp, #-16]!\n    .cfi_def_cfa_offset 16\n    .cfi_offset w29, -16\n    .cfi_offset w30, -8\n    mov x29, sp\n    sub sp, sp, #64\n    .cfi_def_cfa_offset 80\n",e->out);
    addr(e->out,0,"L_path");main_state(e,9);fputs("    str x0, [x9]\n    str xzr, [x9, #8]\n    str xzr, [x9, #16]\n    str xzr, [x9, #24]\n    str wzr, [x9, #48]\n",e->out);
    imm(e->out,0,globals);imm(e->out,1,BPP_CG_VALUE_BYTES);fputs("    bl _bpp_n_allocate_zero\n    cbz x0, L_main_alloc_fail\n",e->out);main_state(e,9);fputs("    str x0, [x9, #16]\n",e->out);
    imm(e->out,0,globals);imm(e->out,1,1);fputs("    bl _bpp_n_allocate_zero\n    cbz x0, L_main_alloc_fail\n",e->out);main_state(e,9);fputs("    str x0, [x9, #24]\n",e->out);
    main_state(e,0);fputs("    ldr x1, [x0, #16]\n    ldr x2, [x0, #24]\n    mov x3, xzr\n    add x4, x0, #32\n",e->out);farjump(e->out,"L_proc_0",1);
    main_state(e,9);fputs("    ldr x0, [x9, #8]\n    cbnz x0, L_main_error\n",e->out);
    if(print){main_state(e,0);fputs("    add x0, x0, #32\n",e->out);fputs(e->truth_states?"    bl _bpp_a_print_result\n":"    bl _bpp_n_print_result\n",e->out);}
    fputs("    b L_main_cleanup\nL_main_error:\n    bl _bpp_n_report_error\n    mov w10, #1\n",e->out);main_state(e,9);fputs("    str w10, [x9, #48]\n    b L_main_cleanup\nL_main_alloc_fail:\n    bl _bpp_n_allocation_failed\n    mov w10, #1\n",e->out);main_state(e,9);fputs("    str w10, [x9, #48]\nL_main_cleanup:\n",e->out);
    main_release_value(e,0,0);
    for(size_t global=0;global<global_count;++global)main_release_value(e,1,global);
    main_state(e,9);fputs("    ldr x0, [x9, #16]\n    bl _bpp_n_free_memory\n",e->out);main_state(e,9);fputs("    ldr x0, [x9, #24]\n    bl _bpp_n_free_memory\n",e->out);main_state(e,9);fputs("    ldr x0, [x9, #8]\n    bl _bpp_n_free_memory\n",e->out);
    main_state(e,9);fputs("    ldr w0, [x9, #48]\n    add sp, sp, #64\n    .cfi_def_cfa_offset 16\n    ldp x29, x30, [sp], #16\n    .cfi_def_cfa_offset 0\n    ret\n    .cfi_endproc\n",e->out);
}
int bl_emit_arm64(BLIRModule *m,FILE *o,int print_result,BPPTarget target) {
    if(!o||!bl_ir_validate_pass(m,"ARM64 backend input")||!bpp_target_validate_ir(m,target,"ARM64"))return 0;
    Layout *layouts=calloc(m->function_count,sizeof *layouts);if(!layouts)return 0;
    BPPRegPlan *plans=calloc(m->function_count,sizeof *plans);
    if(!plans){free(layouts);return 0;}
    for(size_t f=0;f<m->function_count;++f) {
        if(!bpp_reg_plan_create_target(&m->functions[f],&plans[f],target)) {
            snprintf(m->error,sizeof m->error,"ARM64 register allocation failed in function '%s': %s",m->functions[f].name,bpp_reg_plan_last_error(&plans[f]));
            for(size_t j=0;j<m->function_count;++j)bpp_reg_plan_destroy(&plans[j]);
            free(plans);free(layouts);return 0;
        }
        if(!bpp_reg_plan_validate(&m->functions[f],&plans[f])) {
            snprintf(m->error,sizeof m->error,"ARM64 register-map verifier failed in function '%s'",m->functions[f].name);
            for(size_t j=0;j<m->function_count;++j)bpp_reg_plan_destroy(&plans[j]);
            free(plans);free(layouts);return 0;
        }
        if(!layout(&m->functions[f],&plans[f],&layouts[f])) {
            frame_error(m,&m->functions[f]);
        for(size_t j=0;j<m->function_count;++j)bpp_reg_plan_destroy(&plans[j]);
        free(plans);free(layouts);return 0;
        }
    }
    size_t maxregs=0;
    for(size_t f=0;f<m->function_count;++f)if(m->functions[f].register_count>maxregs)maxregs=m->functions[f].register_count;
    unsigned char *dirty=calloc(maxregs?maxregs:1,1),*before=calloc(maxregs?maxregs:1,1),*kind=calloc(maxregs?maxregs:1,1),*constant=calloc(maxregs?maxregs:1,1);unsigned *loaded_phys=calloc(maxregs?maxregs:1,sizeof *loaded_phys),*dirty_phys=calloc(maxregs?maxregs:1,sizeof *dirty_phys);
    long long *value=calloc(maxregs?maxregs:1,sizeof *value);
    if(!dirty||!before||!kind||!constant||!loaded_phys||!dirty_phys||!value){
        for(size_t f=0;f<m->function_count;++f)bpp_reg_plan_destroy(&plans[f]);
        free(dirty);free(before);free(kind);free(constant);free(loaded_phys);free(dirty_phys);free(value);free(plans);free(layouts);return 0;
    }
    bpp_asm_source(o,m);
    fputs("// B++ native codegen; static ABI revision 1 / private frame revision 2\n.section __TEXT,__text,regular,pure_instructions\n.p2align 2\n",o);
    if(m->debug_path&&target==BPP_MACOS_ARM64)fputs("L_bpp_debug_start:\n",o);
    if(!m->emit_library){Emitter launcher={0};launcher.out=o;launcher.target=target;launcher.native_release=1;launcher.truth_states=m->truth_states;emit_native_platform_leaves(o,target);emit_native_main(&launcher,m->global_count,print_result);}
    for(size_t f=0;f<m->function_count;++f){const BLIRFunction *fn=&m->functions[f];Emitter e={0};e.out=o;e.f=f;e.target=target;e.fn=fn;e.layout=layouts[f];e.plan=&plans[f];e.dirty=dirty;e.before=before;e.kind=kind;e.constant=constant;e.loaded_phys=loaded_phys;e.value=value;e.asm_comments=m->emit_asm_comments;e.instrument_native=m->instrument_native;e.native_release=!m->emit_library&&!m->truth_states&&!m->static_library_count;e.truth_states=m->truth_states||m->emit_library||m->static_library_count;e.dirty_phys=dirty_phys;
        if(!bpp_value_facts_create(m,f,&e.facts)){for(size_t j=0;j<m->function_count;++j)bpp_reg_plan_destroy(&plans[j]);free(dirty);free(before);free(kind);free(constant);free(loaded_phys);free(dirty_phys);free(value);free(plans);free(layouts);return 0;}
        e.fact_regs=calloc(fn->register_count?fn->register_count:1,sizeof *e.fact_regs);e.fact_locals=calloc(fn->local_count?fn->local_count:1,sizeof *e.fact_locals);e.fact_globals=calloc(m->global_count?m->global_count:1,sizeof *e.fact_globals);
        if(!e.fact_regs||!e.fact_locals||!e.fact_globals){bpp_value_facts_destroy(&e.facts);free(e.fact_regs);free(e.fact_locals);free(e.fact_globals);for(size_t j=0;j<m->function_count;++j)bpp_reg_plan_destroy(&plans[j]);free(dirty);free(before);free(kind);free(constant);free(loaded_phys);free(dirty_phys);free(value);free(plans);free(layouts);return 0;}
        memset(dirty,0,fn->register_count);memset(dirty_phys,0,fn->register_count*sizeof *dirty_phys);
        fprintf(o,"// payload registers: %zu of %zu IR values cached in x20/x21/x22/x23/x24/x25/x27/x28\n",plans[f].cached_count,fn->register_count);
        if(m->emit_library&&f)fprintf(o,".globl _bpp_v2_%s\n_bpp_v2_%s:\n",fn->name,fn->name);
        if(!m->emit_library&&f)fprintf(o,".p2align 2\n.globl _bpp_source_%s\n_bpp_source_%s:\n",fn->name,fn->name);
        fprintf(o,".p2align 2\nL_proc_%zu:\n    .cfi_startproc\n",f);
        arm_save_prologue(o,&plans[f]);
        fputs("    mov x20, x0\n    mov x21, x1\n    mov x22, x2\n    mov x24, x4\n",o);
        imm(o,9,e.layout.bytes);fprintf(o,"L_probe_%zu:\n    cmp x9, #4096\n    b.lo L_probe_tail_%zu\n    sub sp, sp, #4096\n    str xzr, [sp]\n    sub x9, x9, #4096\n    b L_probe_%zu\nL_probe_tail_%zu:\n    sub sp, sp, x9\n    mov x19, sp\n",f,f,f,f);
        frame_init(&e);if(m->truth_states){fputs("    mov x0, x19\n",o);imm(o,1,f==0?1:0);fputs("    bl _bpp_a_truth_enable\n",o);}if(m->instrument_native)fputs("    ldr x0, [x29, #8]\n    bl _bpp_a_trace_enter\n",o);fprintf(o,"    b L_f%zu_b0\n",f);
            for(size_t b=0;b<fn->block_count;++b){e.b=b;bpp_value_facts_begin_block(&e.facts,b,e.fact_regs,e.fact_locals,e.fact_globals);sync_known(&e);memset(dirty,0,fn->register_count);memset(dirty_phys,0,fn->register_count*sizeof *dirty_phys);memset(loaded_phys,0,fn->register_count*sizeof *loaded_phys);if(!m->instrument_native)for(size_t r=0;r<fn->register_count;++r){unsigned p=bpp_reg_dirty_entry(&plans[f],b,r);if(p){dirty[r]=1;dirty_phys[r]=p;loaded_phys[r]=p;}}fprintf(o,"L_f%zu_b%zu:\n",f,b);if(m->debug_path)fputs("    .cfi_def_cfa x29, 16\n",o);
            for(size_t k=0;k<fn->blocks[b].count;++k){e.k=k;const BLIRInst *i=&fn->blocks[b].insts[k];if(m->debug_path)fprintf(o,"L_bpp_pc_f%zu_b%zu_i%zu_begin:\n",f,b,k);if(i->dst!=BL_IR_NONE&&e.plan->begin[i->dst]==SIZE_MAX){if(m->debug_path)fprintf(o,"L_bpp_pc_f%zu_b%zu_i%zu_end:\n",f,b,k);continue;}if(m->debug_path){fputs("    .cfi_def_cfa x29, 16\n",o);bpp_asm_location(o,m,i);}if(m->emit_asm_comments)bpp_asm_instruction_comment(o,fn,f,b,k,e.plan,target,0);
                if(m->instrument_native&&(i->op==BL_IR_LOAD_GLOBAL||i->op==BL_IR_STORE_GLOBAL||i->op==BL_IR_INIT_GLOBAL)){flush(&e,e.dirty,1);fputs("    mov x0, x19\n",o);imm(o,1,i->op);imm(o,2,i->a);fputs("    bl _bpp_a_access\n",o);}
                if(i->op==BL_IR_JUMP){edge_flush(&e,i->a);if(i->a!=b+1){char target[64];snprintf(target,sizeof target,"L_f%zu_b%zu",f,i->a);farjump(o,target,0);}else if(e.asm_comments)fputs("    // B++ CFG fallthrough jump omitted\n",o);memset(e.dirty,0,fn->register_count);memset(e.dirty_phys,0,fn->register_count*sizeof *e.dirty_phys);{if(m->debug_path)fprintf(o,"L_bpp_pc_f%zu_b%zu_i%zu_end:\n",f,b,k);continue;}}
                if(i->op==BL_IR_RETURN){flush(&e,e.dirty,1);regaddr(&e,12,i->a);fprintf(o,"    str x12, [x19, #%u]\n",BPP_CG_CALL_OUTPUT_OFFSET);char target[64];snprintf(target,sizeof target,"L_cleanup_%zu",f);farjump(o,target,0);{if(m->debug_path)fprintf(o,"L_bpp_pc_f%zu_b%zu_i%zu_end:\n",f,b,k);continue;}}
                if(i->op==BL_IR_BRANCH&&m->truth_states){
                    edge_flush_pair(&e,i->b,i->c);regaddr(&e,12,i->a);
                    fprintf(o,"    ldr w9, [x12]\n    cbz w9, L_state_false_%zu_%zu_%zu\n    cmp w9, #1\n    b.eq L_state_integer_%zu_%zu_%zu\n    ldr x9, [x12, #8]\n    cbz x9, L_state_false_%zu_%zu_%zu\n    ldr w10, [x9]\n    cmp w10, #4\n    b.ne L_state_true_%zu_%zu_%zu\n    ldr d0, [x9, #8]\n    fcmp d0, #0.0\n    b.gt L_state_true_%zu_%zu_%zu\n    b L_state_false_%zu_%zu_%zu\nL_state_integer_%zu_%zu_%zu:\n    ldr x9, [x12, #8]\n    cmp x9, #0\n    b.le L_state_false_%zu_%zu_%zu\nL_state_true_%zu_%zu_%zu:\n",f,b,k,f,b,k,f,b,k,f,b,k,f,b,k,f,b,k,f,b,k,f,b,k,f,b,k);
                    char to[64];snprintf(to,sizeof to,"L_f%zu_b%zu",f,i->b);farjump(o,to,0);fprintf(o,"L_state_false_%zu_%zu_%zu:\n",f,b,k);snprintf(to,sizeof to,"L_f%zu_b%zu",f,i->c);farjump(o,to,0);
                    memset(e.dirty,0,fn->register_count);memset(e.dirty_phys,0,fn->register_count*sizeof *e.dirty_phys);{if(m->debug_path)fprintf(o,"L_bpp_pc_f%zu_b%zu_i%zu_end:\n",f,b,k);continue;}}
                if(i->op==BL_IR_BRANCH){BPPTruth truth=(BPPTruth)e.fact_regs[i->a].truth;if(truth!=BPP_TRUTH_UNKNOWN){size_t to=truth==BPP_TRUTH_TRUE?i->b:i->c;if(e.asm_comments)fprintf(o,"    // B++ truthiness-elided vreg=v%zu truth=%s\n",i->a,bpp_truth_name(truth));edge_flush(&e,to);if(to!=b+1){char target[64];snprintf(target,sizeof target,"L_f%zu_b%zu",f,to);farjump(o,target,0);}memset(e.dirty,0,fn->register_count);memset(e.dirty_phys,0,fn->register_count*sizeof *e.dirty_phys);{if(m->debug_path)fprintf(o,"L_bpp_pc_f%zu_b%zu_i%zu_end:\n",f,b,k);continue;}}if(branch_fallthrough(&e,i)){memset(e.dirty,0,fn->register_count);memset(e.dirty_phys,0,fn->register_count*sizeof *e.dirty_phys);{if(m->debug_path)fprintf(o,"L_bpp_pc_f%zu_b%zu_i%zu_end:\n",f,b,k);continue;}}regaddr(&e,12,i->a);fputs("    ldr w9, [x12]\n",o);branch(&e,"cbz w9,","false");fputs("    cmp w9, #1\n",o);branch(&e,"b.ne","true");payload(&e,9,i->a);fputs("    cmp x9, #0\n",o);branch(&e,"b.gt","true");define(&e,"false");edge_flush(&e,i->c);{char target[64];snprintf(target,sizeof target,"L_f%zu_b%zu",f,i->c);farjump(o,target,0);}define(&e,"true");edge_flush(&e,i->b);{char target[64];snprintf(target,sizeof target,"L_f%zu_b%zu",f,i->b);farjump(o,target,0);}memset(e.dirty,0,fn->register_count);memset(e.dirty_phys,0,fn->register_count*sizeof *e.dirty_phys);{if(m->debug_path)fprintf(o,"L_bpp_pc_f%zu_b%zu_i%zu_end:\n",f,b,k);continue;}}
                /* Every ordinary IR operation has a possible slow helper or
                 * error arm.  Make live homes canonical before the split so
                 * transition reloads can happen lazily after the join. */
                sync_slow_edge_homes(&e);
                memcpy(e.before,e.dirty,fn->register_count);
                int conditional=fast(&e,i);
                if(conditional) define(&e,"slow");
                slow(&e,i,conditional); define(&e,"done");bpp_value_facts_transfer(&e.facts,i,e.fact_regs,e.fact_locals,e.fact_globals);sync_known(&e);if(m->debug_path)fprintf(o,"L_bpp_pc_f%zu_b%zu_i%zu_end:\n",f,b,k);
            }
        }
        if(m->debug_path)fputs("    .cfi_def_cfa x29, 16\n",o);
        fprintf(o,"L_fail_%zu:\n    str xzr, [x19, #%u]\nL_cleanup_%zu:\n    ldr x2, [x19, #%u]\n    ldr x1, [x19, #%u]\n    cbz x1, L_cleanup_null_%zu\n    ldr w9, [x1]\n    ldr x10, [x1, #8]\n    str w9, [x2]\n    str x10, [x2, #8]\n    str wzr, [x1]\n    str xzr, [x1, #8]\n    b L_cleanup_release_%zu\nL_cleanup_null_%zu:\n    str wzr, [x2]\n    str xzr, [x2, #8]\nL_cleanup_release_%zu:\n",f,BPP_CG_CALL_OUTPUT_OFFSET,f,BPP_CG_FRAME_OUTPUT_OFFSET,BPP_CG_CALL_OUTPUT_OFFSET,f,f,f,f);cleanup_frame(&e);
        if(m->instrument_native)fputs("    bl _bpp_a_trace_exit\n",o);
        arm_save_epilogue(o,&plans[f]);if(m->emit_asm_comments){fprintf(o,"    // B++ allocation-traffic function=%s spills=%zu reloads=%zu rematerializations=%zu\n",fn->name,e.emitted_spills,e.emitted_reloads,e.emitted_rematerializations);access_traffic(o,fn);}bpp_value_facts_destroy(&e.facts);free(e.fact_regs);free(e.fact_locals);free(e.fact_globals);
    }
    if(m->debug_path&&target==BPP_MACOS_ARM64)fputs("L_bpp_debug_end:\n",o);
    fputs(".section __TEXT,__cstring,cstring_literals\n",o);literal(o,"L_path",m->path);
    if(!m->emit_library){literal(o,"L_native_newline","\n");}
    for(size_t f=0;f<m->function_count;++f)for(size_t b=0;b<m->functions[f].block_count;++b)for(size_t k=0;k<m->functions[f].blocks[b].count;++k){const BLIRInst *i=&m->functions[f].blocks[b].insts[k];if(i->text){char s[96];snprintf(s,sizeof s,"L_text_%zu_%zu_%zu",f,b,k);literal(o,s,i->text);}}
    if(m->emit_library)for(size_t f=1;f<m->function_count;++f){char label[64];snprintf(label,sizeof label,"L_abi_name_%zu",f);literal(o,label,m->functions[f].name);}
    fputs(".section __TEXT,__const\n.p2align 3\n",o);
    for(size_t f=0;f<m->function_count;++f){
        size_t flags=layout_flags(&plans[f],&m->functions[f]);
        fprintf(o,"L_layout_%zu:\n    .quad %zu,%zu,%zu,%zu,%zu,%zu,%zu,%zu\n",f,m->functions[f].param_count,m->functions[f].register_count,m->functions[f].local_count,layouts[f].maxargs,plans[f].cell_count,plans[f].local_slot_count,plans[f].root_cell_count,flags);
        if(flags&BPP_LAYOUT_REGISTER_MAP)for(size_t r=0;r<m->functions[f].register_count;++r)fprintf(o,"    .quad %zu\n",plans[f].cell[r]);
        if(flags&BPP_LAYOUT_LOCAL_MAP)for(size_t l=0;l<m->functions[f].local_count;++l)fprintf(o,"    .quad %zu\n",plans[f].local_map[l]);
    }
    if(m->emit_library){
        size_t count=m->function_count?m->function_count-1:0;const char *name=m->library_name;
        /* The descriptor and procedure table contain pointers to code and
         * strings.  Mach-O forbids those relocations in __TEXT,__const when
         * a static archive is linked into an executable.  Keep immutable
         * layouts above in __const, but emit ABI metadata in relocatable
         * data. */
        fputs(".section __DATA,__data\n.p2align 3\n",o);
        fprintf(o,".globl _bpp_v2_library_%s\n_bpp_v2_library_%s:\n    .long 0x42505031,2,0,7\n    .long 16,0,8,0\n    .quad %zu\n    .quad _bpp_v2_procedures_%s\n_bpp_v2_procedures_%s:\n",name,name,count,name,name);
        for(size_t f=1;f<m->function_count;++f)fprintf(o,"    .quad L_abi_name_%zu\n    .long %zu,1\n    .quad _bpp_v2_%s\n",f,m->functions[f].param_count,m->functions[f].name);
    }
    fputs(".subsections_via_symbols\n",o);
    if(m->debug_path&&target==BPP_MACOS_ARM64)bpp_asm_compile_unit(o,m,target,"L_bpp_debug_start","L_bpp_debug_end",
                                                                   "L_proc_","L_cleanup_","");
    for(size_t f=0;f<m->function_count;++f)bpp_reg_plan_destroy(&plans[f]);
    free(dirty);free(before);free(kind);free(constant);free(loaded_phys);free(dirty_phys);free(value);free(plans);free(layouts);return !ferror(o);
}
