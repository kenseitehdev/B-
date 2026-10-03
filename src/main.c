#define _POSIX_C_SOURCE 200809L
#include "../include/compiler.h"
#include "../include/target.h"
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <process.h>
#include <io.h>
#include <windows.h>
#define unlink _unlink
#else
#include <unistd.h>
#include <sys/wait.h>
#endif

#ifndef BPP_PROJECT_DIR
#define BPP_PROJECT_DIR "."
#endif
#ifndef BPP_RUNTIME_OBJECT
#define BPP_RUNTIME_OBJECT "build/release/obj/src/runtime.o"
#endif
#ifndef BPP_VALUE_OBJECT
#define BPP_VALUE_OBJECT "build/release/obj/src/value.o"
#endif
#ifndef BPP_NATIVE_RUNTIME_OBJECT
#define BPP_NATIVE_RUNTIME_OBJECT "build/release/obj/src/codegen_runtime.o"
#endif
#ifndef BPP_HOST_CC
#define BPP_HOST_CC "cc"
#endif
#ifndef BPP_SANITIZERS
#define BPP_SANITIZERS ""
#endif
#ifndef BPP_OPTIMIZATION
#define BPP_OPTIMIZATION "-O2"
#endif
static void usage(const char *name) {
    fprintf(stderr, "usage: %s [--check | --emit-ir | --emit-c | --emit-asm | --emit-map] [--print-result] [--backend c|native|arm64|x86_64] [--target TARGET] [--no-opt] [--debug-info] [--asm-comments] [-o output] file.bpp\n       %s --list-targets | --target-info TARGET\n", name,name);
}
static const char *format_name(BPPObjectFormat f){return f==BPP_FORMAT_MACHO?"macho":f==BPP_FORMAT_ELF?"elf":"coff";}
static const char *isa_name(BPPISA i){return i==BPP_ISA_ARM64?"arm64":"x86_64";}
static int target_query(int argc,char **argv){
    if(argc==2&&!strcmp(argv[1],"--list-targets")){for(size_t i=0;i<bpp_target_count();++i){const BPPBackendCapabilities *c=bpp_target_at(i);printf("%s\t%s\t%s\t%s\t%u\t%u\n",c->name,c->triple,format_name(c->format),isa_name(c->isa),c->pointer_bits,c->payload_registers);}return 1;}
    if(argc==3&&!strcmp(argv[1],"--target-info")){BPPTarget t;if(!bpp_target_parse(argv[2],&t))return -1;const BPPBackendCapabilities *c=bpp_target_capabilities(t);printf("target=%s\ntriple=%s\nformat=%s\nisa=%s\npointer_bits=%u\ninteger_bits=%u\npayload_registers=%u\n",c->name,c->triple,format_name(c->format),isa_name(c->isa),c->pointer_bits,c->integer_bits,c->payload_registers);for(int op=0;op<=BL_IR_RETURN;++op)printf("ir.%s=%s\n",bl_ir_op_name((BLIROp)op),bpp_lowering_name(bpp_target_ir_lowering(t,(BLIROp)op)));return 1;}return 0;
}
static int run_command(char *const *command) {
#ifdef _WIN32
    /* The Windows CRT joins spawn arguments without preserving boundaries. */
    char *quoted[32]={0}; size_t count=0;
    for(;count<31&&command[count];++count){
        const char *src=command[count];size_t length=strlen(src);
        if(length>(SIZE_MAX-3)/2)break;
        char *dst=quoted[count]=malloc(length*2+3);if(!dst)break;
        *dst++='"';size_t slashes=0;
        for(;*src;++src){
            if(*src=='\\'){++slashes;continue;}
            size_t copies=*src=='"'?slashes*2+1:slashes;
            while(copies--)*dst++='\\';
            slashes=0;*dst++=*src;
        }
        for(size_t j=0;j<slashes*2;++j)*dst++='\\';
        *dst++='"';*dst=0;
    }
    intptr_t result=command[count]?-1:_spawnvp(_P_WAIT,command[0],(const char *const *)quoted);
    for(size_t j=0;j<count;++j)free(quoted[j]);
    if(result==-1)perror(command[0]);
    return result==0;
#else
    pid_t child = fork();
    if (child < 0) { perror("fork"); return 0; }
    if (!child) { execvp(command[0], command); perror(command[0]); _exit(127); }
    int status;
    while (waitpid(child, &status, 0) < 0) { if (errno == EINTR) continue; perror("waitpid"); return 0; }
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
#endif
}
static int same_file(const char *a,const char *b){
    if(!strcmp(a,b))return 1;
#ifdef _WIN32
    char *full_a=_fullpath(NULL,a,0),*full_b=_fullpath(NULL,b,0);
    int same=full_a&&full_b&&!_stricmp(full_a,full_b);free(full_a);free(full_b);
    if(same)return 1;
#endif
    struct stat sa,sb;
    return !stat(a,&sa)&&!stat(b,&sb)&&sa.st_ino&&sa.st_dev==sb.st_dev&&sa.st_ino==sb.st_ino;
}
#ifdef _WIN32
#define BPP_DEFAULT_OUTPUT "a.exe"
#else
#define BPP_DEFAULT_OUTPUT "a.out"
#endif
static int native_compile(const char *temp, const char *output, int native, BPPTarget target, int object, int debug) {
    const char *cc = getenv("BPP_CC"); if (!cc || !*cc) cc = BPP_HOST_CC;
    char include_path[] = "-I" BPP_PROJECT_DIR "/include";
    char runtime_path[] = BPP_RUNTIME_OBJECT;
    char value_path[] = BPP_VALUE_OBJECT;
    char sanitizer_flag[] = "-fsanitize=" BPP_SANITIZERS;
    char *command[32]; size_t n = 0;
    command[n++] = (char *)cc; if (!native) command[n++] = "-std=c11";
    command[n++] = BPP_OPTIMIZATION;
    if (BPP_SANITIZERS[0]) {
        command[n++] = sanitizer_flag; command[n++] = "-g";
        if(!native){command[n++] = "-fno-omit-frame-pointer"; command[n++] = "-fno-sanitize-recover=undefined";}
    }
    if(debug&&!BPP_SANITIZERS[0])command[n++]="-g";
    command[n++] = include_path;
    const BPPBackendCapabilities *cap=bpp_target_capabilities(target);
    if (native && cap && cap->format==BPP_FORMAT_MACHO && cap->isa==BPP_ISA_ARM64) { command[n++] = "-arch"; command[n++] = "arm64"; }
    if (object) command[n++] = "-c";
    command[n++] = "-x"; command[n++] = native ? "assembler" : "c";
    command[n++] = (char *)temp; command[n++] = "-x"; command[n++] = "none";
    if (!object) {
        command[n++] = runtime_path; command[n++] = value_path;
        if (native) { command[n++] = BPP_NATIVE_RUNTIME_OBJECT; }
    }
    command[n++] = "-o"; command[n++] = (char *)output; command[n] = NULL;
    return run_command(command);
}
int main(int argc, char **argv) {
    int query=target_query(argc,argv);if(query)return query>0?0:2;
    const char *path = NULL, *output = NULL; int mode = 0, print_result = 0, native = 1, optimize=1, debug_info=BPP_SANITIZERS[0]!=0,asm_comments=0; BPPTarget target=bpp_host_target();int explicit_target=0;
    for (int j = 1; j < argc; ++j) {
        if (!strcmp(argv[j], "--help")) { usage(argv[0]); return 0; }
        if(!strcmp(argv[j],"--target")){
            if(++j==argc||!bpp_target_parse(argv[j],&target)){usage(argv[0]);return 2;}explicit_target=1;native=1;continue;
        }
        if(!strcmp(argv[j],"--debug-info")){debug_info=1;continue;}
        if(!strcmp(argv[j],"--asm-comments")){asm_comments=1;continue;}
        if(!strcmp(argv[j],"--no-opt")){optimize=0;continue;}
        if (!strcmp(argv[j], "--backend")) {
            if (++j == argc) { usage(argv[0]); return 2; }
            if(!strcmp(argv[j],"c"))native=0;
            else if(!strcmp(argv[j],"native"))native=1;
            else if(!strcmp(argv[j],"arm64")){native=1;if(!explicit_target)target=bpp_target_default_for_isa(BPP_ISA_ARM64);}
            else if(!strcmp(argv[j],"x86_64")){native=1;if(!explicit_target)target=bpp_target_default_for_isa(BPP_ISA_X86_64);}
            else {usage(argv[0]);return 2;}continue;
        }
        if (!strcmp(argv[j], "--print-result")) { print_result = 1; continue; }
        int selected = !strcmp(argv[j], "--check") ? 1 : !strcmp(argv[j], "--emit-ir") ? 2 : !strcmp(argv[j], "--emit-c") ? 3 : !strcmp(argv[j], "--emit-asm") ? 4 : !strcmp(argv[j],"--emit-map") ? 5 : 0;
        if (selected) { if (mode) { fputs("select only one output mode\n", stderr); return 2; } mode = selected; continue; }
        if (!strcmp(argv[j], "-o")) {
            if (output || ++j == argc) { usage(argv[0]); return 2; } output = argv[j]; continue;
        }
        if (argv[j][0] == '-' || path) { fprintf(stderr, "unexpected argument: %s\n", argv[j]); return 2; }
        path = argv[j];
    }
    if (!path || (mode == 1 && output) || (asm_comments && mode != 4)) { usage(argv[0]); return 2; }
    if(!mode&&native&&!bpp_target_is_host(target)){
        fprintf(stderr,"native executables require a matching host and runtime objects; use --emit-asm --target %s for cross-target output\n",bpp_target_name(target));return 2;
    }
    const char *destination = output ? output : (mode ? NULL : BPP_DEFAULT_OUTPUT);
    if (destination && same_file(path,destination)) {
        fputs("output must differ from source\n", stderr); return 2;
    }
    BLCompiler compiler = {0}; BLIRModule ir; bl_ir_init(&ir);
    int ok = bl_compile_file(&compiler, path, &ir);
    ir.emit_asm_comments=asm_comments;
    if(ok&&optimize)ok=bpp_ir_optimize(&ir);
    ir.instrument_native=BPP_SANITIZERS[0]!=0;
    if(ok&&debug_info&&ir.expanded_source&&(mode==4||mode==5||(!mode&&native))){
        const char *base=output?output:(!mode?BPP_DEFAULT_OUTPUT:NULL);
        if(base){size_t size=strlen(base)+32;ir.debug_path=calloc(size,1);
            if(!ir.debug_path)ok=0;
            else{snprintf(ir.debug_path,size,"%s.bpp-source",base);FILE *source=NULL;
                if(same_file(path,ir.debug_path))fputs("debug snapshot must differ from source\n",stderr);
                else source=fopen(ir.debug_path,"w");
                if(!source)ok=0;else{if(fputs(ir.expanded_source,source)==EOF)ok=0;if(fclose(source))ok=0;}}
        }
    }
    if (!ok) fprintf(stderr, "%s\n", compiler.last_error ? compiler.last_error : "compile error");
    if (ok && mode != 1) {
        FILE *out = NULL;
        if (mode >= 2) {
            out = output ? fopen(output, "w") : stdout;
            if (!out) { perror(output); ok = 0; }
            else {
                ok = mode == 2 ? bl_ir_dump(&ir, out) : mode == 3 ? bl_emit_c(&ir, out, print_result) : mode==5?bpp_emit_locations(&ir,out,target):bpp_emit_native(&ir,out,print_result,target);
                if (output) { if (fclose(out)) ok = 0; } else if (fflush(out)) ok = 0;
            }
        } else {
            char temp[4096];
#ifdef _WIN32
            char directory[MAX_PATH];DWORD count=GetTempPathA(MAX_PATH,directory);
            FILE *out_temp=NULL;
            if(count&&count<MAX_PATH&&GetTempFileNameA(directory,"bpp",0,temp))out_temp=fopen(temp,"w");
#else
            snprintf(temp,sizeof temp,"/tmp/bpp-code-XXXXXX");int fd=mkstemp(temp);FILE *out_temp=fd<0?NULL:fdopen(fd,"w");
            if(fd>=0&&!out_temp)close(fd);
#endif
            if(!out_temp){perror("temporary output");ok=0;}
            else {ok=native?bpp_emit_native(&ir,out_temp,print_result,target):bl_emit_c(&ir,out_temp,print_result);if(fclose(out_temp))ok=0;}
            if(ok)ok=native_compile(temp,output?output:BPP_DEFAULT_OUTPUT,native,target,0,debug_info);
            if(out_temp)unlink(temp);
        }
        if (!ok) fprintf(stderr, "B++: output generation failed%s%s\n", ir.error[0] ? ": " : "", ir.error);
    }
    bl_ir_dispose(&ir); bl_compiler_dispose(&compiler); return ok ? 0 : 1;
}
