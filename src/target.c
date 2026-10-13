#include "../include/target.h"
#include "../include/compiler.h"
#include "../include/isel.h"
#include <stdlib.h>
#include <limits.h>
#include <string.h>

/* All limits are byte displacements.  ADRP rounds both addresses down to a
 * 4 KiB page before applying its signed 21-bit page displacement. */
static const BPPRangeLimit arm64_macho[] = {
    {BPP_RANGE_ARM64_ADRP_PAGE,"arm64-adrp-page","ARM64_RELOC_PAGE21/ARM64_RELOC_PAGEOFF12",-4294967296LL,4294963200LL},
    {BPP_RANGE_ARM64_LITERAL19,"arm64-literal19","assembler-local imm19 (no Mach-O relocation)",-1048576LL,1048572LL},
    {BPP_RANGE_ARM64_BRANCH26,"arm64-branch26","ARM64_RELOC_BRANCH26",-134217728LL,134217724LL},
    {BPP_RANGE_ARM64_COND19,"arm64-cond19","assembler-local imm19 (no Mach-O relocation)",-1048576LL,1048572LL}
};
static const BPPRangeLimit arm64_elf[] = {
    {BPP_RANGE_ARM64_ADRP_PAGE,"arm64-adrp-page","R_AARCH64_ADR_PREL_PG_HI21/R_AARCH64_ADD_ABS_LO12_NC",-4294967296LL,4294963200LL},
    {BPP_RANGE_ARM64_LITERAL19,"arm64-literal19","R_AARCH64_LD_PREL_LO19",-1048576LL,1048572LL},
    {BPP_RANGE_ARM64_BRANCH26,"arm64-branch26","R_AARCH64_JUMP26/R_AARCH64_CALL26",-134217728LL,134217724LL},
    {BPP_RANGE_ARM64_COND19,"arm64-cond19","R_AARCH64_CONDBR19",-1048576LL,1048572LL}
};
static const BPPRangeLimit x64_elf[] = {
    {BPP_RANGE_X64_REL32,"x86-64-rel32","R_X86_64_PC32/R_X86_64_PLT32",-2147483648LL,2147483647LL},
    {BPP_RANGE_X64_ABS64,"x86-64-abs64","R_X86_64_64",LLONG_MIN,LLONG_MAX}
};
static const BPPRangeLimit x64_coff[] = {
    {BPP_RANGE_X64_REL32,"x86-64-rel32","IMAGE_REL_AMD64_REL32",-2147483648LL,2147483647LL},
    {BPP_RANGE_X64_ABS64,"x86-64-abs64","IMAGE_REL_AMD64_ADDR64",LLONG_MIN,LLONG_MAX}
};

static const BPPBackendCapabilities targets[] = {
    {BPP_MACOS_ARM64,"macos-arm64","arm64-apple-macosx13.0.0",BPP_ISA_ARM64,BPP_FORMAT_MACHO,64,64,11,
     "_",".section __TEXT,__text,regular,pure_instructions",".section __TEXT,__cstring,cstring_literals",".section __TEXT,__const",
     ".section __DATA,__data",".section __DATA,__bss","", "aapcs64-darwin","dwarf-cfi/compact-unwind","",16,0,0,4096,arm64_macho,4},
    {BPP_LINUX_X64,"linux-x86_64","x86_64-unknown-linux-gnu",BPP_ISA_X86_64,BPP_FORMAT_ELF,64,64,6,
     "",".text",".section .rodata",".section .rodata",".data",".bss",".section .note.GNU-stack,\"\",@progbits",
     "system-v-amd64","dwarf-cfi","",16,0,128,4096,x64_elf,2},
    {BPP_LINUX_ARM64,"linux-arm64","aarch64-unknown-linux-gnu",BPP_ISA_ARM64,BPP_FORMAT_ELF,64,64,11,
     "",".text",".section .rodata",".section .rodata",".data",".bss",".section .note.GNU-stack,\"\",%progbits",
     "aapcs64","dwarf-cfi","",16,0,0,4096,arm64_elf,4},
    {BPP_WINDOWS_X64,"windows-x86_64","x86_64-w64-windows-gnu",BPP_ISA_X86_64,BPP_FORMAT_COFF,64,64,6,
     "",".text",".section .rdata,\"dr\"",".section .rdata,\"dr\"",".data",".bss","",
     "windows-x64","seh","___chkstk_ms",16,32,0,4096,x64_coff,2}
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
const char *bpp_target_name(BPPTarget t){const BPPBackendCapabilities *c=bpp_target_capabilities(t);return c?c->name:t==BPP_TARGET_UNSUPPORTED?"unsupported-host":"unknown-target";}
int bpp_target_parse(const char *s,BPPTarget *t){if(!s||!t)return 0;for(size_t i=0;i<bpp_target_count();++i)if(!strcmp(s,targets[i].name)){*t=targets[i].id;return 1;}return 0;}
BPPTarget bpp_target_for_host(const char *os,const char *architecture){
 if(!os||!architecture)return BPP_TARGET_UNSUPPORTED;
 int arm=!strcmp(architecture,"arm64")||!strcmp(architecture,"aarch64");
 int x64=!strcmp(architecture,"x86_64")||!strcmp(architecture,"AMD64");
 if(!strcmp(os,"macos")&&arm)return BPP_MACOS_ARM64;
 if(!strcmp(os,"linux")&&arm)return BPP_LINUX_ARM64;
 if(!strcmp(os,"linux")&&x64)return BPP_LINUX_X64;
 if(!strcmp(os,"windows")&&x64)return BPP_WINDOWS_X64;
 return BPP_TARGET_UNSUPPORTED;
}
BPPTarget bpp_host_target(void){
#if defined(_WIN32)
 const char *os="windows";
#elif defined(__APPLE__)
 const char *os="macos";
#elif defined(__linux__)
 const char *os="linux";
#else
 const char *os="unsupported";
#endif
#if defined(__aarch64__) || defined(__arm64__) || defined(_M_ARM64)
 const char *architecture="arm64";
#elif defined(__x86_64__) || defined(_M_X64)
 const char *architecture="x86_64";
#else
 const char *architecture="unsupported";
#endif
 return bpp_target_for_host(os,architecture);
}
int bpp_host_supported(void){return bpp_target_capabilities(bpp_host_target())!=NULL;}
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
/* ARM instruction selection is shared. This adapter changes object-format
 * directives, symbol decoration, and page relocations, never instructions. */
int bpp_emit_native(BLIRModule *m,FILE *out,int print,BPPTarget t){
 const BPPBackendCapabilities *c=bpp_target_capabilities(t);if(!c||!bpp_target_validate_ir(m,t,"native"))return 0;
 /* Ordinary programs now use the raw-word backend.  Archive ABI v1 remains
  * a separately quarantined compatibility path until its BLValue boundary is
  * replaced by the raw static ABI revision. */
 if(!m->truth_states&&!m->emit_library&&!m->static_library_count&&!m->static_procedure_count)
  return bpp_emit_raw_native(m,out,print,t);
 if(m->strict_native_runtime)return bpp_emit_strict_native_runtime(m,out,print,t);
 if(c->isa==BPP_ISA_X86_64)return bpp_emit_x64(m,out,print,c->format==BPP_FORMAT_COFF);
 if(c->format==BPP_FORMAT_MACHO)return bl_emit_arm64(m,out,print,t);
 FILE *scratch=tmpfile();if(!scratch)return 0;
 int ok=bl_emit_arm64(m,scratch,print,t);rewind(scratch);char line[16384];
 while(ok&&fgets(line,sizeof line,scratch)){
  if(!strncmp(line,".file ",6)){fputs(line,out);continue;}
  if(!strncmp(line,".section __TEXT,__text",sizeof(".section __TEXT,__text")-1)){fputs(".text\n",out);continue;}
  if(!strncmp(line,".section __TEXT,__cstring",sizeof(".section __TEXT,__cstring")-1)||!strncmp(line,".section __TEXT,__const",sizeof(".section __TEXT,__const")-1)){fputs(".section .rodata\n",out);continue;}
  if(!strncmp(line,".section __DATA,__data",sizeof(".section __DATA,__data")-1)){fputs(".data\n",out);continue;}
  if(!strncmp(line,".subsections_via_symbols",23)){fputs(".section .note.GNU-stack,\"\",%progbits\n",out);continue;}
  char *p;while((p=strstr(line,"_bpp_")))memmove(p,p+1,strlen(p));while((p=strstr(line,"_main")))memmove(p,p+1,strlen(p));
  if((p=strstr(line,"@PAGEOFF"))){char *start=p;while(start>line&&start[-1]!=' ')--start;*p='\0';fprintf(out,"%.*s:lo12:%s%s",(int)(start-line),line,start,p+8);continue;}
  if((p=strstr(line,"@PAGE")))memmove(p,p+5,strlen(p+5)+1);
  fputs(line,out);
 }
 if(ferror(scratch))ok=0;
 fclose(scratch);return ok&&!ferror(out);
}
