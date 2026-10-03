#include "../include/target.h"
#include "../include/regalloc.h"
#include "../include/codegen_abi.h"
static void quote(FILE *o,const char *s){fputc('"',o);for(const unsigned char *p=(const unsigned char *)s;*p;++p){if(*p=='"'||*p=='\\')fputc('\\',o);
    if(*p<32)fprintf(o,"\\u%04x",*p);else fputc(*p,o);}fputc('"',o);}
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
        fputs("],\"instructions\":[",o);int comma=0;
        for(size_t b=0;b<m->functions[f].block_count;++b)for(size_t k=0;k<m->functions[f].blocks[b].count;++k){BLIRInst *i=&m->functions[f].blocks[b].insts[k];
    if(comma++)fputc(',',o);
    fprintf(o,"{\"block\":%zu,\"instruction\":%zu,\"line\":%d,\"column\":%d}",b,k,i->line,i->column);}
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
    for(size_t x=0;x<n;++x){BLIRId r=ids[x];if(r>=p->register_count)continue;fprintf(o," vreg=v%zu:phys=%s:spill-cell=%zu",r,phys_name(target,p->physical[r]),p->cell[r]);}
    if(i->op==BL_IR_CALL)for(size_t x=0;x<i->arg_count;++x){BLIRId r=i->args[x];if(r<p->register_count)fprintf(o," arg%zu:vreg=v%zu:phys=%s:spill-cell=%zu",x,r,phys_name(target,p->physical[r]),p->cell[r]);}
    fputc('\n',o);
}
