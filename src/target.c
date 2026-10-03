#include "../include/target.h"
#include "../include/compiler.h"
#include <stdlib.h>
#include <string.h>
const char *bpp_target_name(BPPTarget t) {
    static const char *names[]={"macos-arm64","linux-x86_64","linux-arm64","windows-x86_64"};
    return names[t];
}
int bpp_target_parse(const char *s,BPPTarget *t) {
    for(unsigned i=0;i<4;++i)if(!strcmp(s,bpp_target_name((BPPTarget)i))){*t=(BPPTarget)i;return 1;}
    return 0;
}
BPPTarget bpp_host_target(void) {
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
int bpp_host_supported(void) {
#if (defined(__APPLE__) && defined(__aarch64__)) || (defined(__linux__) && (defined(__x86_64__) || defined(__aarch64__))) || (defined(_WIN32) && (defined(__x86_64__) || defined(_M_X64)))
    return 1;
#else
    return 0;
#endif
}
/* The ARM64 instruction selection is shared. This adapter changes object-format
 * directives, symbol decoration, and page relocations, never instructions. */
int bpp_emit_native(BLIRModule *m,FILE *out,int print,BPPTarget t) {
    if(t==BPP_LINUX_X64||t==BPP_WINDOWS_X64)return bpp_emit_x64(m,out,print,t==BPP_WINDOWS_X64);
    if(t==BPP_MACOS_ARM64)return bl_emit_arm64(m,out,print);
    FILE *scratch=tmpfile();if(!scratch)return 0;
    int ok=bl_emit_arm64(m,scratch,print);
    rewind(scratch);char line[16384];
    while(ok&&fgets(line,sizeof line,scratch)) {
        if(!strncmp(line,".file ",6)){fputs(line,out);continue;}
        if(!strncmp(line,".section __TEXT,__text",sizeof(".section __TEXT,__text")-1)){fputs(".text\n",out);continue;}
        if(!strncmp(line,".section __TEXT,__cstring",sizeof(".section __TEXT,__cstring")-1)||!strncmp(line,".section __TEXT,__const",sizeof(".section __TEXT,__const")-1)){fputs(".section .rodata\n",out);continue;}
        if(!strncmp(line,".subsections_via_symbols",23)){fputs(".section .note.GNU-stack,\"\",%progbits\n",out);continue;}
        char *p;
        while((p=strstr(line,"_bpp_")))memmove(p,p+1,strlen(p));
        while((p=strstr(line,"_main")))memmove(p,p+1,strlen(p));
        if((p=strstr(line,"@PAGEOFF"))) {
            char *start=p;while(start>line&&start[-1]!=' ')--start;
            *p='\0';fprintf(out,"%.*s:lo12:%s%s",(int)(start-line),line,start,p+8);continue;
        }
        if((p=strstr(line,"@PAGE")))memmove(p,p+5,strlen(p+5)+1);
        fputs(line,out);
    }
    if(ferror(scratch))ok=0;
    fclose(scratch);return ok&&!ferror(out);
}
