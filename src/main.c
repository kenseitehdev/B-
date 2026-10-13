#define _POSIX_C_SOURCE 200809L
#include "../include/compiler.h"
#include "../include/regalloc.h"
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
#ifndef BPP_TRUTH_OBJECT
#define BPP_TRUTH_OBJECT "build/release/obj/src/truth.o"
#endif
#ifndef BPP_STATIC_ABI_OBJECT
#define BPP_STATIC_ABI_OBJECT "build/release/obj/src/bpp_abi.o"
#endif
#ifndef BPP_HOST_CC
#define BPP_HOST_CC "cc"
#endif
#ifndef BPP_NATIVE_SANITIZER_OBJECT
#define BPP_NATIVE_SANITIZER_OBJECT "build/debug/obj/src/native_sanitizer.o"
#endif
#ifndef BPP_SANITIZERS
#define BPP_SANITIZERS ""
#endif
#ifndef BPP_OPTIMIZATION
#define BPP_OPTIMIZATION "-O2"
#endif
static void usage(const char *name) {
    fprintf(stderr, "usage: %s [--check | --emit-ir | --emit-asm | --emit-map | --emit-link-map | --emit-library-manifest | --emit-dependencies] [--print-result] [--backend native|arm64|x86_64] [--target TARGET] [-L directory] [--alloc-profile FILE] [--no-opt] [--debug-info] [--asm-comments] [--strict-native-runtime] [--native-sanitizers] [--emit-library --library-name NAME] [-o output] [file.bpp]\n       %s --list-targets | --target-info TARGET | --build-info\n\nWith no source path, B++ compiles ./main.bpp and requires define proc main().\n", name,name);
}
static const char *format_name(BPPObjectFormat f){return f==BPP_FORMAT_MACHO?"macho":f==BPP_FORMAT_ELF?"elf":"coff";}
static const char *isa_name(BPPISA i){return i==BPP_ISA_ARM64?"arm64":"x86_64";}
static int abi_identifier(const char *name){if(!name||!(*name=='_'||(*name>='A'&&*name<='Z')||(*name>='a'&&*name<='z')))return 0;for(++name;*name;++name)if(!(*name=='_'||(*name>='A'&&*name<='Z')||(*name>='a'&&*name<='z')||(*name>='0'&&*name<='9')))return 0;return 1;}
static void json_string(FILE *out,const char *value){
    fputc('"',out);
    for(const unsigned char *p=(const unsigned char *)value;*p;++p){
        if(*p=='"'||*p=='\\'){fputc('\\',out);fputc(*p,out);}
        else if(*p<32)fprintf(out,"\\u%04x",*p);
        else fputc(*p,out);
    }
    fputc('"',out);
}
static void build_info(void){
    fputs("{\"schema\":1,\"host_target\":",stdout);json_string(stdout,bpp_target_name(bpp_host_target()));
    fprintf(stdout,",\"host_supported\":%s,\"sanitizers\":",bpp_host_supported()?"true":"false");json_string(stdout,BPP_SANITIZERS);
    fputs(",\"host_cc\":",stdout);json_string(stdout,BPP_HOST_CC);
    const char *objects[]={BPP_RUNTIME_OBJECT,BPP_VALUE_OBJECT,BPP_NATIVE_RUNTIME_OBJECT,BPP_STATIC_ABI_OBJECT,BPP_TRUTH_OBJECT};
    fputs(",\"runtime_objects\":[",stdout);
    for(size_t i=0;i<sizeof objects/sizeof objects[0];++i){if(i)fputc(',',stdout);json_string(stdout,objects[i]);}
    fputs("]}\n",stdout);
}
static int target_query(int argc,char **argv){
    if(argc==2&&!strcmp(argv[1],"--build-info")){build_info();return 1;}
    if(argc==2&&!strcmp(argv[1],"--list-targets")){for(size_t i=0;i<bpp_target_count();++i){const BPPBackendCapabilities *c=bpp_target_at(i);printf("%s\t%s\t%s\t%s\t%u\t%u\n",c->name,c->triple,format_name(c->format),isa_name(c->isa),c->pointer_bits,c->allocatable_payload_registers);}return 1;}
    if(argc==3&&!strcmp(argv[1],"--target-info")){BPPTarget t;if(!bpp_target_parse(argv[2],&t))return -1;const BPPBackendCapabilities *c=bpp_target_capabilities(t);size_t count=0,range_count=0;const BPPRegisterInfo *regs=bpp_target_register_inventory(t,&count);const BPPRangeLimit *ranges=bpp_target_range_limits(t,&range_count);printf("target=%s\ntriple=%s\nformat=%s\nisa=%s\npointer_bits=%u\ninteger_bits=%u\nallocatable_payload_registers=%u\nregister_inventory_count=%zu\nrange_limit_count=%zu\n",c->name,c->triple,format_name(c->format),isa_name(c->isa),c->pointer_bits,c->integer_bits,c->allocatable_payload_registers,count,range_count);for(size_t r=0;r<count;++r)printf("register.%s=id:%u,class:%u,width:%u,flags:%u,save_cost:%u,call_clobbers:%u\n",regs[r].name,regs[r].id,regs[r].class_mask,regs[r].width_bits,regs[r].flags,regs[r].save_cost,regs[r].call_clobbers);for(size_t r=0;r<range_count;++r)printf("range.%s=relocation:%s,min:%lld,max:%lld\n",ranges[r].name,ranges[r].relocation,ranges[r].minimum,ranges[r].maximum);for(int op=0;op<BL_IR_OPCODE_COUNT;++op)printf("ir.%s=%s\n",bl_ir_op_name((BLIROp)op),bpp_lowering_name(bpp_target_ir_lowering(t,(BLIROp)op)));return 1;}return 0;
}
static int verification_trace(void){const char *enabled=getenv("BPP_VERIFICATION_TRACE");return enabled&&*enabled;}
static int run_command(char *const *command) {
    if(verification_trace()){fprintf(stderr,"B++ verification: host tool %s\n",command[0]);fflush(stderr);}
#ifdef _WIN32
    /* The Windows CRT joins spawn arguments without preserving boundaries. */
    size_t count=0; while(command[count])++count;
    char **quoted=calloc(count+1,sizeof *quoted); if(!quoted)return 0;
    size_t built=0;
    for(;built<count;++built){
        const char *src=command[built];size_t length=strlen(src);
        if(length>(SIZE_MAX-3)/2)break;
        char *dst=quoted[built]=malloc(length*2+3);if(!dst)break;
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
    intptr_t result=built!=count?-1:_spawnvp(_P_WAIT,command[0],(const char *const *)quoted);
    for(size_t j=0;j<built;++j)free(quoted[j]);free(quoted);
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
static int is_main_source(const char *path) {
    if (!path) return 0;
    const char *base = strrchr(path, '/');
#ifdef _WIN32
    const char *backslash = strrchr(path, '\\');
    if (!base || (backslash && backslash > base)) base = backslash;
#endif
    base = base ? base + 1 : path;
    return strcmp(base, "main.bpp") == 0;
}
static int archive_identity(const char *path, size_t *size_out, unsigned long long *fingerprint_out) {
    FILE *file = fopen(path, "rb"); if (!file) return 0;
    if (fseek(file, 0, SEEK_END) != 0) { fclose(file); return 0; }
    long size = ftell(file); if (size < 0 || fseek(file, 0, SEEK_SET) != 0) { fclose(file); return 0; }
    unsigned long long hash = UINT64_C(1469598103934665603); unsigned char bytes[4096]; size_t count;
    while ((count = fread(bytes, 1, sizeof bytes, file)) != 0)
        for (size_t i = 0; i < count; ++i) { hash ^= bytes[i]; hash *= UINT64_C(1099511628211); }
    int ok = !ferror(file) && fclose(file) == 0; if (!ok) return 0;
    if (size_out) *size_out = (size_t)size;
    if (fingerprint_out) *fingerprint_out = hash;
    return 1;
}
static char *library_archive_path(const char *manifest_output, const char *name) {
    const char *slash = manifest_output ? strrchr(manifest_output, '/') : NULL;
    size_t directory = slash ? (size_t)(slash - manifest_output + 1) : 0;
    size_t bytes = directory + strlen("lib") + strlen(name) + strlen(".bpp.a") + 1;
    char *path = calloc(bytes, 1); if (!path) { perror("calloc"); return NULL; }
    if (directory) memcpy(path, manifest_output, directory);
    snprintf(path + directory, bytes - directory, "lib%s.bpp.a", name); return path;
}
#ifdef _WIN32
#define BPP_DEFAULT_OUTPUT "a.exe"
#else
#define BPP_DEFAULT_OUTPUT "a.out"
#endif
static int native_compile(const char *temp, const char *output, BPPTarget target, int object, int debug, const BLIRModule *module) {
    if(verification_trace()){fputs("B++ verification: native assembly ready, invoking host driver\n",stderr);fflush(stderr);}
    const char *cc = getenv("BPP_CC"); if (!cc || !*cc) cc = BPP_HOST_CC;
    char runtime_path[] = BPP_RUNTIME_OBJECT;
    char value_path[] = BPP_VALUE_OBJECT;
    char sanitizer_flag[] = "-fsanitize=" BPP_SANITIZERS;
    int msan = strcmp(BPP_SANITIZERS, "memory") == 0;
    const BPPBackendCapabilities *cap=bpp_target_capabilities(target);
    int macho_debug=!object&&debug&&cap&&cap->format==BPP_FORMAT_MACHO;
    int coff_debug=!object&&debug&&cap&&cap->format==BPP_FORMAT_COFF;
    char pdb_flag[4096];
    if(coff_debug){int length=snprintf(pdb_flag,sizeof pdb_flag,"-Wl,--pdb=%s.pdb",output);
        if(length<0||(size_t)length>=sizeof pdb_flag){fputs("B++: PDB output path is too long\n",stderr);return 0;}}
    char object_temp[4096],dsym_path[4096];
    if(macho_debug){
        int object_length=snprintf(object_temp,sizeof object_temp,"%s.o",temp);
        int dsym_length=snprintf(dsym_path,sizeof dsym_path,"%s.dSYM",output);
        if(object_length<0||(size_t)object_length>=sizeof object_temp||dsym_length<0||(size_t)dsym_length>=sizeof dsym_path){fputs("B++: debug output path is too long\n",stderr);return 0;}
    }
    size_t capacity = 32 + (module ? module->static_library_count + module->static_link_flag_count : 0);
    char **command = calloc(capacity, sizeof *command); if (!command) { perror("calloc"); return 0; }
    size_t n = 0;
    /* Apple's linker leaves assembler DWARF in its debug map rather than
     * embedding it in the executable. Keep the object alive through dsymutil
     * so the final dSYM contains the line table and LLDB can resolve sources. */
    if(macho_debug){
        command[n++]=(char *)cc;
        if(BPP_SANITIZERS[0]){command[n++]=sanitizer_flag;command[n++]="-g";}
        if(!BPP_SANITIZERS[0])command[n++]="-g";
        if(cap->isa==BPP_ISA_ARM64){command[n++]="-arch";command[n++]="arm64";}
        command[n++]="-c";
        command[n++]="-x";command[n++]="assembler";command[n++]=(char *)temp;
        command[n++]="-o";command[n++]=object_temp;command[n]=NULL;
        if(!run_command(command)){free(command);unlink(object_temp);return 0;}
        n=0;
    }
    command[n++] = (char *)cc;
    if(coff_debug){command[n++]="-fuse-ld=lld";command[n++]=pdb_flag;}
    if(!macho_debug)command[n++] = BPP_OPTIMIZATION;
    if (BPP_SANITIZERS[0]) {
        command[n++] = sanitizer_flag; command[n++] = "-g";
        if(cap&&cap->format==BPP_FORMAT_COFF&&strstr(BPP_SANITIZERS,"undefined"))command[n++]="-fsanitize-trap=undefined";
    }
    if(debug&&!BPP_SANITIZERS[0])command[n++]="-g";
    if (cap && cap->format==BPP_FORMAT_MACHO && cap->isa==BPP_ISA_ARM64) { command[n++] = "-arch"; command[n++] = "arm64"; }
    /* The x86-64 large internal code model uses R_X86_64_64 for B++ labels.
     * Generated ELF executables therefore deliberately use the non-PIE static
     * model until the linker stage owns PLT/GOT and veneer placement. */
    if (cap && cap->format==BPP_FORMAT_ELF && cap->isa==BPP_ISA_X86_64) command[n++] = "-no-pie";
    if (object) command[n++] = "-c";
    if(macho_debug)command[n++]=object_temp;
    else {command[n++] = "-x"; command[n++] = "assembler";command[n++] = (char *)temp;}
    if (!object) {
        /* -x selects the generated source only. Runtime objects and archives
         * must be passed back to the driver's ordinary input detection. */
        int legacy_native_boundary = module && (module->truth_states || module->emit_library || module->static_library_count || module->static_procedure_count);
        if(legacy_native_boundary||(module&&module->instrument_native)){
            command[n++]="-x";command[n++]="none";
        }
        /* The strict bootstrap profile is deliberately linkable without any
         * B++ C runtime object. Do not weaken this by adding an implicit
         * compatibility object here. */
        /* Raw-word executables call only the host allocation/output boundary;
         * they do not link runtime.o, value.o, codegen_runtime.o, or the
         * legacy static-ABI bridge.  Archive v1 remains transitional. */
        if (legacy_native_boundary) {
            command[n++] = runtime_path; command[n++] = value_path;
            command[n++] = BPP_NATIVE_RUNTIME_OBJECT; command[n++] = BPP_STATIC_ABI_OBJECT; command[n++]=BPP_TRUTH_OBJECT;
        }
        if(module && module->instrument_native && !legacy_native_boundary)
            command[n++] = BPP_NATIVE_SANITIZER_OBJECT;
        /* Manifest resolution records dependencies before dependants.  Reverse
         * that order for traditional one-pass static archive linkers. */
        if (module) for (size_t i = module->static_library_count; i-- > 0;) command[n++] = module->static_libraries[i].archive_path;
        if (module) for (size_t i = 0; i < module->static_link_flag_count; ++i) command[n++] = module->static_link_flags[i];
        if (msan) command[n++] = "-pie";
    }
    command[n++] = "-o"; command[n++] = (char *)output; command[n] = NULL;
    int ok = run_command(command);
    if(ok&&macho_debug){
        char *dsym_command[]={"dsymutil",(char *)output,"-o",dsym_path,NULL};
        ok=run_command(dsym_command);
        if(!ok)fprintf(stderr,"B++: dsymutil failed while creating '%s'\n",dsym_path);
    }
    if(macho_debug){
        const char *keep_debug_object=getenv("BPP_KEEP_DEBUG_OBJECT");
        if(keep_debug_object&&*keep_debug_object)
            fprintf(stderr,"B++: preserved Mach-O debug object: %s\n",object_temp);
        else unlink(object_temp);
    }
    free(command); return ok;
}

static int emit_library_manifest(const BLIRModule *ir, FILE *out, const char *name, BPPTarget target,
                                 size_t archive_size, unsigned long long archive_fingerprint) {
    if (!ir || !out || !name) return 0;
    fprintf(out, "# B++ static-library manifest; generated by bpp\nbppabi 1\nname %s\nabi 2.0\nruntime 3\narchive %s lib%s.bpp.a %zu %016llx\n", name, bpp_target_name(target), name, archive_size, archive_fingerprint);
    for (size_t i = 0; i < ir->static_library_count; ++i) fprintf(out, "dependency %s\n", ir->static_libraries[i].name);
    for (size_t f = 1; f < ir->function_count; ++f) fprintf(out, "export %s %zu\n", ir->functions[f].name, ir->functions[f].param_count);
    return !ferror(out);
}
static int emit_link_map(const BLIRModule *ir, FILE *out, BPPTarget target) {
    if (!ir || !out) return 0;
    fprintf(out, "bpp-link-map 1\ntarget %s\nlibraries %zu\n", bpp_target_name(target), ir->static_library_count);
    for (size_t i = 0; i < ir->static_library_count; ++i) {
        const BPPStaticLibraryImport *library = &ir->static_libraries[i];
        fprintf(out, "library %zu name=%s\n  manifest=%s\n  archive=%s\n  bytes=%zu\n  fnv1a64=%016llx\n", i,
                library->name, library->manifest_path, library->archive_path,
                library->archive_size, library->archive_fingerprint);
    }
    fprintf(out, "procedures %zu\n", ir->static_procedure_count);
    for (size_t i = 0; i < ir->static_procedure_count; ++i) {
        const BPPStaticProcedureImport *procedure = &ir->static_procedures[i];
        fprintf(out, "procedure %zu name=%s symbol=%s arity=%zu library=%zu\n", i,
                procedure->name, procedure->symbol, procedure->arity, procedure->library_index);
    }
    fprintf(out, "system_libraries %zu\n", ir->static_link_flag_count);
    for (size_t i = 0; i < ir->static_link_flag_count; ++i)
        fprintf(out, "system_library %zu flag=%s\n", i, ir->static_link_flags[i]);
    return !ferror(out);
}
static int emit_dependencies(const BLIRModule *ir,FILE *out){
    fputs("{\"schema\":1,\"sources\":[",out);
    for(size_t i=0;i<ir->source_file_count;++i){if(i)fputc(',',out);json_string(out,ir->source_files[i].path);}
    fputs("],\"manifests\":[",out);
    for(size_t i=0;i<ir->static_library_count;++i){if(i)fputc(',',out);json_string(out,ir->static_libraries[i].manifest_path);}
    fputs("],\"archives\":[",out);
    for(size_t i=0;i<ir->static_library_count;++i){if(i)fputc(',',out);json_string(out,ir->static_libraries[i].archive_path);}
    fputs("]}\n",out);return !ferror(out);
}
int main(int argc, char **argv) {
    int query=target_query(argc,argv);if(query)return query>0?0:2;
    const char *path = NULL, *output = NULL, *alloc_profile=NULL,*library_name=NULL; int mode = 0, print_result = 0, optimize=1, debug_info=BPP_SANITIZERS[0]!=0,asm_comments=0,emit_library=0,emit_library_manifest_mode=0,strict_native_runtime=0,native_checks=0; BPPTarget target=bpp_host_target();int explicit_target=0;
    const char **library_paths = NULL; size_t library_path_count = 0, library_path_capacity = 0;
    for (int j = 1; j < argc; ++j) {
        if (!strcmp(argv[j], "--help")) { usage(argv[0]); return 0; }
        if(!strcmp(argv[j],"--target")){
            if(++j==argc||!bpp_target_parse(argv[j],&target)){usage(argv[0]);return 2;}explicit_target=1;continue;
        }
        if(!strcmp(argv[j],"--debug-info")){debug_info=1;continue;}
        if(!strcmp(argv[j],"--native-sanitizers")){native_checks=1;debug_info=1;continue;}
        if(!strcmp(argv[j],"--strict-native-runtime")){strict_native_runtime=1;continue;}
        if(!strcmp(argv[j],"--asm-comments")){asm_comments=1;continue;}
        if(!strcmp(argv[j],"--emit-library")){emit_library=1;continue;}
        if(!strcmp(argv[j],"--emit-library-manifest")){if(mode){fputs("select only one output mode\n",stderr);free(library_paths);return 2;}mode=6;emit_library_manifest_mode=1;continue;}
        if(!strcmp(argv[j],"--library-name")){if(++j==argc||!abi_identifier(argv[j])){fputs("library name must be an ASCII identifier\n",stderr);return 2;}library_name=argv[j];continue;}
        if(!strcmp(argv[j],"-L")){if(++j==argc||!argv[j][0]){usage(argv[0]);free(library_paths);return 2;}if(library_path_count==library_path_capacity){size_t cap=library_path_capacity?library_path_capacity*2:4;const char **items=realloc(library_paths,cap*sizeof *items);if(!items){perror("realloc");free(library_paths);return 1;}library_paths=items;library_path_capacity=cap;}library_paths[library_path_count++]=argv[j];continue;}
        if(!strcmp(argv[j],"--alloc-profile")){if(++j==argc){usage(argv[0]);return 2;}alloc_profile=argv[j];continue;}
        if(!strcmp(argv[j],"--no-opt")){optimize=0;continue;}
        if (!strcmp(argv[j], "--backend")) {
            if (++j == argc) { usage(argv[0]); free(library_paths); return 2; }
            if(!strcmp(argv[j],"native"))continue;
            else if(!strcmp(argv[j],"arm64")){if(!explicit_target)target=bpp_target_default_for_isa(BPP_ISA_ARM64);}
            else if(!strcmp(argv[j],"x86_64")){if(!explicit_target)target=bpp_target_default_for_isa(BPP_ISA_X86_64);}
            else {usage(argv[0]);free(library_paths);return 2;}continue;
        }
        if (!strcmp(argv[j], "--print-result")) { print_result = 1; continue; }
        int selected = !strcmp(argv[j], "--check") ? 1 : !strcmp(argv[j], "--emit-ir") ? 2 : !strcmp(argv[j], "--emit-asm") ? 4 : !strcmp(argv[j],"--emit-map") ? 5 : !strcmp(argv[j],"--emit-link-map") ? 7 : !strcmp(argv[j],"--emit-dependencies") ? 8 : 0;
        if (selected) { if (mode) { fputs("select only one output mode\n", stderr); free(library_paths); return 2; } mode = selected; continue; }
        if (!strcmp(argv[j], "-o")) {
            if (output || ++j == argc) { usage(argv[0]); return 2; } output = argv[j]; continue;
        }
        if (argv[j][0] == '-' || path) { fprintf(stderr, "unexpected argument: %s\n", argv[j]); free(library_paths); return 2; }
        path = argv[j];
    }
    if (!path && !emit_library && !emit_library_manifest_mode) path = "main.bpp";
    if (!path || (mode == 1 && output) || (asm_comments && mode != 4) || (emit_library&&mode!=4&&mode!=8) || (emit_library&&!library_name) || (emit_library_manifest_mode&&!library_name) || (!emit_library&&!emit_library_manifest_mode&&library_name) || (native_checks&&(strict_native_runtime||emit_library))) { usage(argv[0]); free(library_paths); return 2; }
    if(!mode&&!bpp_target_is_host(target)){
        fprintf(stderr,"native executables require a matching host and runtime objects; use --emit-asm --target %s for cross-target output\n",bpp_target_name(target));free(library_paths);return 2;
    }
    const char *destination = output ? output : (mode ? NULL : BPP_DEFAULT_OUTPUT);
    if (destination && same_file(path,destination)) {
        fputs("output must differ from source\n", stderr); free(library_paths); return 2;
    }
    if(alloc_profile){char profile_error[512];if(!bpp_reg_profile_load(alloc_profile,profile_error,sizeof profile_error)){fprintf(stderr,"B++: invalid allocation profile: %s\n",profile_error);free(library_paths);return 2;}}
    BLCompiler compiler = {0}; compiler.target=target; compiler.library_paths=library_paths; compiler.library_path_count=library_path_count; BLIRModule ir; bl_ir_init(&ir);
    int ok = bl_compile_file(&compiler, path, &ir);
    if (ok && !emit_library && !emit_library_manifest_mode) {
        int declares_main = 0;
        for (size_t f = 1; f < ir.function_count; ++f)
            if (strcmp(ir.functions[f].name, "main") == 0) { declares_main = 1; break; }
        if (is_main_source(path) || declares_main) ok = bl_ir_require_zero_argument_main(&ir);
    }
    ir.emit_asm_comments=asm_comments;
    ir.emit_library=emit_library||emit_library_manifest_mode;
    ir.strict_native_runtime=strict_native_runtime;
    ir.library_name=library_name;
    ir.truth_states=!strict_native_runtime&&!native_checks;
    /* The optimizer runs before the debug snapshot path is allocated.  Give
     * it the intent explicitly so source accesses remain visible in debug
     * output and sanitizer-instrumented native builds. */
    /* The explicitly requested bootstrap profile promises helper-free output.
     * Sanitized raw output requires the checked-memory C boundary instead. */
    ir.instrument_native=(BPP_SANITIZERS[0]!=0||native_checks)&&!strict_native_runtime;
    ir.preserve_access_observability=debug_info;
    for(size_t f=0;f<ir.function_count;++f)ir.functions[f].preserve_source_homes=debug_info;
    if(ok&&native_checks)ok=bpp_raw_native_eligible(&ir);
    if(ok&&verification_trace()){fputs("B++ verification: optimize IR\n",stderr);fflush(stderr);}
    if(ok&&optimize&&mode!=8)ok=bpp_ir_optimize(&ir);
    if(ok&&verification_trace()){fputs("B++ verification: IR ready\n",stderr);fflush(stderr);}
    if(ok&&debug_info&&ir.expanded_source&&(mode==4||mode==5||(!mode))){
        const char *base=output?output:(!mode?BPP_DEFAULT_OUTPUT:NULL);
        if(base){size_t size=strlen(base)+32;ir.debug_path=calloc(size,1);
            if(!ir.debug_path)ok=0;
            else{snprintf(ir.debug_path,size,"%s.bpp-source",base);FILE *source=NULL;
                if(same_file(path,ir.debug_path))fputs("debug snapshot must differ from source\n",stderr);
                else source=fopen(ir.debug_path,"w");
                if(!source)ok=0;else{if(fputs(ir.expanded_source,source)==EOF)ok=0;if(fclose(source))ok=0;}}
        }
    }
    if (!ok) fprintf(stderr, "%s\n", compiler.last_error ? compiler.last_error : (ir.error[0] ? ir.error : "compile error"));
    if(ok&&mode==8){
        FILE *out=output?fopen(output,"w"):stdout;
        if(!out){perror(output);ok=0;}
        else{ok=emit_dependencies(&ir,out);if(output&&fclose(out))ok=0;else if(!output&&fflush(out))ok=0;}
    } else if (ok && mode == 6) {
        char *archive = library_archive_path(output, library_name); size_t archive_size = 0; unsigned long long archive_fingerprint = 0;
        if (!archive || !archive_identity(archive, &archive_size, &archive_fingerprint)) { fprintf(stderr, "B++: cannot create manifest: expected archive '%s' beside the manifest\n", archive ? archive : "<allocation failed>"); ok = 0; }
        FILE *out = ok ? (output ? fopen(output, "w") : stdout) : NULL;
        if (ok && !out) { perror(output); ok = 0; }
        else if (ok) { ok = emit_library_manifest(&ir, out, library_name, target, archive_size, archive_fingerprint); if (output && fclose(out)) ok = 0; else if (!output && fflush(out)) ok = 0; }
        free(archive);
    } else if (ok && mode == 7) {
        FILE *out = output ? fopen(output, "w") : stdout;
        if (!out) { perror(output); ok = 0; }
        else { ok = emit_link_map(&ir, out, target); if (output && fclose(out)) ok = 0; else if (!output && fflush(out)) ok = 0; }
    } else if (ok && mode != 1) {
        FILE *out = NULL;
        if (mode >= 2) {
            out = output ? fopen(output, "w") : stdout;
            if (!out) { perror(output); ok = 0; }
            else {
                ok = mode == 2 ? bl_ir_dump(&ir, out) : mode==5?(!ir.truth_states&&bpp_raw_native_eligible(&ir)?bpp_emit_raw_locations(&ir,out,target):bpp_emit_locations(&ir,out,target)):bpp_emit_native(&ir,out,print_result,target);
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
            else {ok=bpp_emit_native(&ir,out_temp,print_result,target);if(fclose(out_temp))ok=0;}
            if(ok)ok=native_compile(temp,output?output:BPP_DEFAULT_OUTPUT,target,0,debug_info,&ir);
            if(out_temp)unlink(temp);
        }
        if (!ok) fprintf(stderr, "B++: output generation failed%s%s\n", ir.error[0] ? ": " : "", ir.error);
    }
    bl_ir_dispose(&ir); bl_compiler_dispose(&compiler);bpp_reg_profile_clear(); free(library_paths); return ok ? 0 : 1;
}
