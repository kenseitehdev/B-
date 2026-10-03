#include "../include/target.h"
#include "../include/regalloc.h"
#include "../include/codegen_abi.h"
static void quote(FILE *o,const char *s){fputc('"',o);for(const unsigned char *p=(const unsigned char *)s;*p;++p){if(*p=='"'||*p=='\\')fputc('\\',o);
    if(*p<32)fprintf(o,"\\u%04x",*p);else fputc(*p,o);}fputc('"',o);}
int bpp_emit_locations(BLIRModule *m,FILE *o,BPPTarget t){
    if(!bl_ir_validate(m))return 0;
    fputs("{\"target\":",o);quote(o,bpp_target_name(t));
    fputs(",\"source\":",o);quote(o,m->path);
    fputs(",\"functions\":[",o);
    for(size_t f=0;f<m->function_count;++f){BPPRegPlan p;
    if(!bpp_reg_plan_create_for(&m->functions[f],&p,t==BPP_LINUX_X64||t==BPP_WINDOWS_X64?4:8))return 0;
        if(f)fputc(',',o);
    fputs("{\"name\":",o);quote(o,m->functions[f].name);fprintf(o,",\"label\":\"L_proc_%zu\",\"frame_register\":\"%s\",\"value_cells\":%zu,\"locals\":[",f,t==BPP_LINUX_X64||t==BPP_WINDOWS_X64?"r12":"x19",p.cell_count);
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
