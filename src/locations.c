#include "../include/target.h"
#include "../include/regalloc.h"
#include "../include/codegen_abi.h"
static const char *phys_name(BPPTarget t,unsigned p);
static void quote(FILE *o,const char *s){fputc('"',o);for(const unsigned char *p=(const unsigned char *)s;*p;++p){if(*p=='"'||*p=='\\')fputc('\\',o);
    if(*p<32)fprintf(o,"\\u%04x",*p);else fputc(*p,o);}fputc('"',o);}
static void clobbers(FILE *o,unsigned flags){
    fputc('[',o);int comma=0;
#define CLOBBER(bit,name) do{if(flags&(bit)){if(comma++)fputc(',',o);quote(o,name);}}while(0)
    CLOBBER(BPP_CLOBBER_HELPER,"helper");CLOBBER(BPP_CLOBBER_USER_CALL,"user-call");
    CLOBBER(BPP_CLOBBER_BUILTIN,"builtin");CLOBBER(BPP_CLOBBER_RUNTIME_ERROR,"runtime-error");
    CLOBBER(BPP_CLOBBER_RETURN_CLEANUP,"return-cleanup");CLOBBER(BPP_CLOBBER_SANITIZER,"sanitizer");
#undef CLOBBER
    fputc(']',o);
}
int bpp_emit_locations(BLIRModule *m,FILE *o,BPPTarget t){
    if(!bl_ir_validate_pass(m,"location-map backend input"))return 0;
    fputs("{\"target\":",o);quote(o,bpp_target_name(t));
    fputs(",\"source\":",o);quote(o,m->path);
    fputs(",\"functions\":[",o);
    for(size_t f=0;f<m->function_count;++f){BPPRegPlan p;
    const BPPBackendCapabilities *cap=bpp_target_capabilities(t);
    if(!cap||!bpp_reg_plan_create_target(&m->functions[f],&p,t)){
        snprintf(m->error,sizeof m->error,"location-map register allocation failed in function '%s'",m->functions[f].name);return 0;
    }
    if(!bpp_reg_plan_validate(&m->functions[f],&p)){
        bpp_reg_plan_destroy(&p);snprintf(m->error,sizeof m->error,"location-map register-map verifier failed in function '%s'",m->functions[f].name);return 0;
    }
        if(f)fputc(',',o);
    fputs("{\"name\":",o);quote(o,m->functions[f].name);fprintf(o,",\"label\":\"L_proc_%zu\",\"frame_register\":\"%s\",\"value_cells\":%zu,\"locals\":[",f,cap->isa==BPP_ISA_X86_64?"r12":"x19",p.cell_count);
        for(size_t j=0;j<m->functions[f].local_count;++j){if(j)fputc(',',o);
    fputs("{\"name\":",o);quote(o,m->functions[f].locals[j]);fprintf(o,",\"offset\":%zu}",BPP_CG_HEADER_BYTES+p.cell_count*16+j*16);}
        fputs("],\"allocation_segments\":[",o);int comma=0;
        for(size_t r=0;r<p.register_count;++r)for(size_t j=p.segment_offsets[r];j<p.segment_offsets[r+1];++j){const BPPRegSegment *seg=&p.segments[j];if(comma++)fputc(',',o);
            BPPRegLocation loc=bpp_reg_location_at(&p,r,seg->begin);
            const char *kind=loc.kind==BPP_LOC_REGISTER?"register":loc.kind==BPP_LOC_CONSTANT?"constant":loc.kind==BPP_LOC_REMATERIALIZABLE?"rematerializable":"spill";
            fprintf(o,"{\"vreg\":%zu,\"begin\":%zu,\"end\":%zu,\"kind\":",r,seg->begin,seg->end);quote(o,kind);
            if(loc.kind==BPP_LOC_REGISTER){fputs(",\"physical\":",o);quote(o,phys_name(t,loc.physical));}
            if(loc.kind==BPP_LOC_CONSTANT||loc.kind==BPP_LOC_REMATERIALIZABLE)fprintf(o,",\"immediate\":%lld",loc.immediate);
            if(seg->kind==BPP_LOC_SPILL){fputs(",\"spill_reason\":",o);quote(o,bpp_reg_segment_spill_reason_name((BPPRegSegmentSpillReason)seg->spill_reason));}
            fprintf(o,",\"spill_cell\":%zu}",loc.spill_cell);}
        fputs("],\"instructions\":[",o);int instruction_comma=0;
        for(size_t b=0;b<m->functions[f].block_count;++b)for(size_t k=0;k<m->functions[f].blocks[b].count;++k){BLIRInst *i=&m->functions[f].blocks[b].insts[k];
    if(instruction_comma++)fputc(',',o);
    size_t pos=bpp_reg_position(&p,b,k);fprintf(o,"{\"block\":%zu,\"instruction\":%zu,\"position\":%zu,\"line\":%d,\"column\":%d,\"clobbers\":",b,k,pos,i->line,i->column);clobbers(o,bpp_reg_clobber_at(&p,b,k));
    fputs(",\"spill_candidates\":[",o);int spill_comma=0;for(size_t r=0;r<p.register_count;++r){BPPSpillReason reason=bpp_reg_spill_reason_before(&p,b,k,r);BPPRegLocation loc=bpp_reg_location_at(&p,r,pos);if(reason==BPP_SPILL_NONE||loc.kind!=BPP_LOC_REGISTER)continue;if(spill_comma++)fputc(',',o);fprintf(o,"{\"vreg\":%zu,\"physical\":",r);quote(o,phys_name(t,loc.physical));fprintf(o,",\"spill_cell\":%zu,\"reason\":",loc.spill_cell);quote(o,bpp_reg_spill_reason_name(reason));fputc('}',o);}fputs("]}",o);}
        fputs("]}",o);bpp_reg_plan_destroy(&p);
    }fputs("]}\n",o);return !ferror(o);
}

void bpp_asm_source(FILE *o,const BLIRModule *m){if(!m->debug_path)return;fputs(".file 1 ",o);quote(o,m->debug_path?m->debug_path:m->path);fputc('\n',o);}
void bpp_asm_location(FILE *o,const BLIRInst *i){if(i->line>0)fprintf(o,"    .loc 1 %d %d\n",i->line,i->column>0?i->column:0);}
static const char *phys_name(BPPTarget t,unsigned p){
    static char x64[32];const BPPBackendCapabilities *c=bpp_target_capabilities(t);const BPPRegisterInfo *r=bpp_target_register(t,p);if(!c||!p)return "spill";if(!r)return "?";
    if(c->isa==BPP_ISA_ARM64)return r->name;
    snprintf(x64,sizeof x64,"%%%s",r->name);return x64;
}
void bpp_asm_instruction_comment(FILE *o,const BLIRFunction *fn,size_t fi,size_t bi,size_t ki,const BPPRegPlan *p,BPPTarget target,int intel){
    (void)fi;
    if(!o||!fn||!p||bi>=fn->block_count||ki>=fn->blocks[bi].count)return;
    const BLIRInst *i=&fn->blocks[bi].insts[ki];fprintf(o,intel?"    # B++ opcode=%s function=%s block=%zu source=%d:%d":"    // B++ opcode=%s function=%s block=%zu source=%d:%d",bl_ir_op_name(i->op),fn->name,bi,i->line,i->column);
    BLIRId ids[4];size_t n=0;if(i->dst!=BL_IR_NONE)ids[n++]=i->dst;
    switch(i->op){case BL_IR_NEG:case BL_IR_NOT:case BL_IR_TO_STRING_BUFFER:case BL_IR_RELEASE:case BL_IR_BRANCH:case BL_IR_RETURN:ids[n++]=i->a;break;
    case BL_IR_ADD:case BL_IR_SUB:case BL_IR_MUL:case BL_IR_DIV:case BL_IR_MOD:case BL_IR_EQ:case BL_IR_NE:case BL_IR_LT:case BL_IR_LE:case BL_IR_GT:case BL_IR_GE:case BL_IR_INDEX_GET:ids[n++]=i->a;ids[n++]=i->b;break;
    case BL_IR_INDEX_SET:ids[n++]=i->a;ids[n++]=i->b;ids[n++]=i->c;break;default:break;}
    if(i->op==BL_IR_INIT_LOCAL||i->op==BL_IR_INIT_GLOBAL||i->op==BL_IR_STORE_LOCAL||i->op==BL_IR_STORE_GLOBAL)ids[n++]=i->b;
    size_t pos=bpp_reg_position(p,bi,ki);
    for(size_t x=0;x<n;++x){BLIRId r=ids[x];if(r>=p->register_count)continue;BPPRegLocation loc=bpp_reg_location_at(p,r,pos);
        if(loc.kind==BPP_LOC_REGISTER)fprintf(o," vreg=v%zu:phys=%s:spill-cell=%zu",r,phys_name(target,loc.physical),loc.spill_cell);
        else if(loc.kind==BPP_LOC_CONSTANT)fprintf(o," vreg=v%zu:constant=%lld",r,loc.immediate);
        else if(loc.kind==BPP_LOC_REMATERIALIZABLE)fprintf(o," vreg=v%zu:remat=%lld:spill-cell=%zu",r,loc.immediate,loc.spill_cell);
        else if(loc.kind==BPP_LOC_SPILL)fprintf(o," vreg=v%zu:phys=spill:spill-cell=%zu",r,loc.spill_cell);
        else fprintf(o," vreg=v%zu:location=dead",r);}
    if(i->op==BL_IR_CALL)for(size_t x=0;x<i->arg_count;++x){BLIRId r=i->args[x];if(r<p->register_count){BPPRegLocation loc=bpp_reg_location_at(p,r,pos);fprintf(o," arg%zu:vreg=v%zu:location=%s",x,r,loc.kind==BPP_LOC_REGISTER?phys_name(target,loc.physical):loc.kind==BPP_LOC_CONSTANT?"constant":"spill");}}
    fputc('\n',o);
}
