#define _POSIX_C_SOURCE 200809L
#include "../include/compiler.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/stat.h>

#ifndef BPP_PROJECT_DIR
#define BPP_PROJECT_DIR "."
#endif
#ifndef BPP_RUNTIME_OBJECT
#define BPP_RUNTIME_OBJECT "build/release/obj/src/runtime.o"
#endif
#ifndef BPP_VALUE_OBJECT
#define BPP_VALUE_OBJECT "build/release/obj/src/value.o"
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
    fprintf(stderr, "usage: %s [--check | --emit-ir | --emit-c] [--print-result] [-o output] file.bpp\n", name);
}
static int native_compile(const char *temp, const char *output) {
    const char *cc = getenv("BPP_CC"); if (!cc || !*cc) cc = BPP_HOST_CC;
    char include_path[] = "-I" BPP_PROJECT_DIR "/include";
    char runtime_path[] = BPP_RUNTIME_OBJECT;
    char value_path[] = BPP_VALUE_OBJECT;
    char sanitizer_flag[] = "-fsanitize=" BPP_SANITIZERS;
    char *command[24]; size_t n = 0;
    command[n++] = (char *)cc; command[n++] = "-std=c11";
    command[n++] = BPP_OPTIMIZATION;
    if (BPP_SANITIZERS[0]) {
        command[n++] = sanitizer_flag; command[n++] = "-g";
        command[n++] = "-fno-omit-frame-pointer"; command[n++] = "-fno-sanitize-recover=undefined";
    }
    command[n++] = include_path; command[n++] = "-x"; command[n++] = "c";
    command[n++] = (char *)temp; command[n++] = "-x"; command[n++] = "none"; command[n++] = runtime_path; command[n++] = value_path;
    command[n++] = "-o"; command[n++] = (char *)output; command[n] = NULL;
    pid_t child = fork();
    if (child < 0) { perror("fork"); return 0; }
    if (!child) { execvp(cc, command); perror(cc); _exit(127); }
    int status;
    while (waitpid(child, &status, 0) < 0) { if (errno == EINTR) continue; perror("waitpid"); return 0; }
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}
int main(int argc, char **argv) {
    const char *path = NULL, *output = NULL; int mode = 0, print_result = 0;
    for (int j = 1; j < argc; ++j) {
        if (!strcmp(argv[j], "--help")) { usage(argv[0]); return 0; }
        if (!strcmp(argv[j], "--print-result")) { print_result = 1; continue; }
        int selected = !strcmp(argv[j], "--check") ? 1 : !strcmp(argv[j], "--emit-ir") ? 2 : !strcmp(argv[j], "--emit-c") ? 3 : 0;
        if (selected) { if (mode) { fputs("select only one output mode\n", stderr); return 2; } mode = selected; continue; }
        if (!strcmp(argv[j], "-o")) {
            if (output || ++j == argc) { usage(argv[0]); return 2; } output = argv[j]; continue;
        }
        if (argv[j][0] == '-' || path) { fprintf(stderr, "unexpected argument: %s\n", argv[j]); return 2; }
        path = argv[j];
    }
    if (!path || (mode == 1 && output)) { usage(argv[0]); return 2; }
    const char *destination = output ? output : (mode ? NULL : "a.out");
    struct stat source_stat, output_stat;
    if (destination && ((!strcmp(path, destination)) ||
        (!stat(path, &source_stat) && !stat(destination, &output_stat) &&
         source_stat.st_dev == output_stat.st_dev && source_stat.st_ino == output_stat.st_ino))) {
        fputs("output must differ from source\n", stderr); return 2;
    }
    BLCompiler compiler = {0}; BLIRModule ir; bl_ir_init(&ir);
    int ok = bl_compile_file(&compiler, path, &ir);
    if (!ok) fprintf(stderr, "%s\n", compiler.last_error ? compiler.last_error : "compile error");
    if (ok && mode != 1) {
        FILE *out = NULL;
        if (mode == 2 || mode == 3) {
            out = output ? fopen(output, "w") : stdout;
            if (!out) { perror(output); ok = 0; }
            else {
                ok = mode == 2 ? bl_ir_dump(&ir, out) : bl_emit_c(&ir, out, print_result);
                if (output) { if (fclose(out)) ok = 0; } else if (fflush(out)) ok = 0;
            }
        } else {
            char temp[] = "/tmp/bpp-code-XXXXXX"; int fd = mkstemp(temp);
            if (fd < 0) { perror("mkstemp"); ok = 0; }
            else {
                out = fdopen(fd, "w");
                if (!out) { perror("fdopen"); close(fd); ok = 0; }
                else { ok = bl_emit_c(&ir, out, print_result); if (fclose(out)) ok = 0; }
                if (ok) ok = native_compile(temp, output ? output : "a.out");
                unlink(temp);
            }
        }
        if (!ok) fprintf(stderr, "B++: output generation failed%s%s\n", ir.error[0] ? ": " : "", ir.error);
    }
    bl_ir_dispose(&ir); bl_compiler_dispose(&compiler); return ok ? 0 : 1;
}
