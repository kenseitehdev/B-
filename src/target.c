#include "../include/target.h"
#include "../include/compiler.h"
#include "../include/isel.h"
#include <stdlib.h>
#include <string.h>

static const BPPBackendCapabilities targets[] = {
    {BPP_MACOS_ARM64,"macos-arm64","arm64-apple-macosx13.0.0",BPP_ISA_ARM64,BPP_FORMAT_MACHO,64,64,11},
    {BPP_LINUX_X64,"linux-x86_64","x86_64-unknown-linux-gnu",BPP_ISA_X86_64,BPP_FORMAT_ELF,64,64,6},
    {BPP_LINUX_ARM64,"linux-arm64","aarch64-unknown-linux-gnu",BPP_ISA_ARM64,BPP_FORMAT_ELF,64,64,11},
    {BPP_WINDOWS_X64,"windows-x86_64","x86_64-w64-windows-gnu",BPP_ISA_X86_64,BPP_FORMAT_COFF,64,64,6}
};
size_t bpp_target_count(void){return sizeof targets/sizeof targets[0];}
const BPPBackendCapabilities *bpp_target_at(size_t i){return i<bpp_target_count()?&targets[i]:NULL;}
const BPPBackendCapabilities *bpp_target_capabilities(BPPTarget t){return (unsigned)t<bpp_target_count()?&targets[t]:NULL;}
BPPTarget bpp_target_default_for_isa(BPPISA isa){
 const BPPBackendCapabilities *host=bpp_target_capabilities(bpp_host_target());if(host&&host->isa==isa)return host->id;
 for(size_t i=0;i<bpp_target_count();++i)if(targets[i].isa==isa&&targets[i].format==BPP_FORMAT_ELF)return targets[i].id;
 for(size_t i=0;i<bpp_target_count();++i)if(targets[i].isa==isa)return targets[i].id;
 return bpp_host_target();
}
const char *bpp_target_name(BPPTarget t){const BPPBackendCapabilities *c=bpp_target_capabilities(t);return c?c->name:"unknown-target";}
int bpp_target_parse(const char *s,BPPTarget *t){if(!s||!t)return 0;for(size_t i=0;i<bpp_target_count();++i)if(!strcmp(s,targets[i].name)){*t=targets[i].id;return 1;}return 0;}
BPPTarget bpp_host_target(void){
#if defined(_WIN32)
 return BPP_WINDOWS_X64;
#elif defined(__APPLE__)
 return BPP_MACOS_ARM64;
#elif defined(__aarch64__)
 return BPP_LINUX_ARM64;
#else
 return BPP_LINUX_X64;
#endif
}
int bpp_host_supported(void){
#if (defined(__APPLE__) && defined(__aarch64__)) || (defined(__linux__) && (defined(__x86_64__) || defined(__aarch64__))) || (defined(_WIN32) && (defined(__x86_64__) || defined(_M_X64)))
 return 1;
#else
 return 0;
#endif
}
int bpp_target_is_host(BPPTarget t){return bpp_host_supported()&&t==bpp_host_target();}
const char *bpp_lowering_name(BPPLowering x){static const char *n[]={"unsupported","native","runtime","mixed"};return (unsigned)x<4?n[x]:"unsupported";}
int bpp_target_validate_ir(BLIRModule *m,BPPTarget t,const char *backend){
 if(!m||!bpp_target_capabilities(t))return 0;
 for(size_t f=0;f<m->function_count;++f)for(size_t b=0;b<m->functions[f].block_count;++b)for(size_t k=0;k<m->functions[f].blocks[b].count;++k){const BLIRInst *i=&m->functions[f].blocks[b].insts[k];if(bpp_target_ir_lowering(t,i->op)==BPP_LOWER_UNSUPPORTED){snprintf(m->error,sizeof m->error,"%s:%d:%d: %s backend does not support IR opcode '%s' for target '%s' (function '%s', block %zu, instruction %zu)",m->path?m->path:"<input>",i->line,i->column,backend?backend:"native",bl_ir_op_name(i->op),bpp_target_name(t),m->functions[f].name,b,k);return 0;}}
 return 1;
}
BPPLowering bpp_target_ir_lowering(BPPTarget target,BLIROp op){
 const BPPISelRule *rule=bpp_isel_rule(target,op);
 return rule?rule->lowering:BPP_LOWER_UNSUPPORTED;
}
int bpp_c_ir_supported(BLIROp op){
 switch(op){
 case BL_IR_NULL:case BL_IR_INT:case BL_IR_STRING:case BL_IR_LOAD_LOCAL:case BL_IR_STORE_LOCAL:
 case BL_IR_LOAD_GLOBAL:case BL_IR_STORE_GLOBAL:case BL_IR_INIT_LOCAL:case BL_IR_INIT_GLOBAL:
 case BL_IR_NEG:case BL_IR_NOT:case BL_IR_ADD:case BL_IR_SUB:case BL_IR_MUL:case BL_IR_DIV:case BL_IR_MOD:
 case BL_IR_EQ:case BL_IR_NE:case BL_IR_LT:case BL_IR_LE:case BL_IR_GT:case BL_IR_GE:
 case BL_IR_INDEX_GET:case BL_IR_INDEX_SET:case BL_IR_TO_STRING_BUFFER:case BL_IR_CALL:case BL_IR_RELEASE:
 case BL_IR_FREE_LOCAL:case BL_IR_FREE_GLOBAL:case BL_IR_DELETE_LOCAL:case BL_IR_DELETE_GLOBAL:
 case BL_IR_JUMP:case BL_IR_BRANCH:case BL_IR_RETURN:return 1;
 }
 return 0;
}
/* ARM instruction selection is shared. This adapter changes object-format
 * directives, symbol decoration, and page relocations, never instructions. */
int bpp_emit_native(BLIRModule *m,FILE *out,int print,BPPTarget t){
 const BPPBackendCapabilities *c=bpp_target_capabilities(t);if(!c||!bpp_target_validate_ir(m,t,"native"))return 0;
 if(c->isa==BPP_ISA_X86_64)return bpp_emit_x64(m,out,print,c->format==BPP_FORMAT_COFF);
 if(c->format==BPP_FORMAT_MACHO)return bl_emit_arm64(m,out,print,t);
 FILE *scratch=tmpfile();if(!scratch)return 0;
 int ok=bl_emit_arm64(m,scratch,print,t);rewind(scratch);char line[16384];
 while(ok&&fgets(line,sizeof line,scratch)){
  if(!strncmp(line,".file ",6)){fputs(line,out);continue;}
  if(!strncmp(line,".section __TEXT,__text",sizeof(".section __TEXT,__text")-1)){fputs(".text\n",out);continue;}
  if(!strncmp(line,".section __TEXT,__cstring",sizeof(".section __TEXT,__cstring")-1)||!strncmp(line,".section __TEXT,__const",sizeof(".section __TEXT,__const")-1)){fputs(".section .rodata\n",out);continue;}
  if(!strncmp(line,".subsections_via_symbols",23)){fputs(".section .note.GNU-stack,\"\",%progbits\n",out);continue;}
  char *p;while((p=strstr(line,"_bpp_")))memmove(p,p+1,strlen(p));while((p=strstr(line,"_main")))memmove(p,p+1,strlen(p));
  if((p=strstr(line,"@PAGEOFF"))){char *start=p;while(start>line&&start[-1]!=' ')--start;*p='\0';fprintf(out,"%.*s:lo12:%s%s",(int)(start-line),line,start,p+8);continue;}
  if((p=strstr(line,"@PAGE")))memmove(p,p+5,strlen(p+5)+1);
  fputs(line,out);
 }
 if(ferror(scratch))ok=0;
 fclose(scratch);return ok&&!ferror(out);
}
